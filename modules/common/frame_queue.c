#include "frame_queue.h"            /* frame_packet / frame_queue。 */

#include <stdlib.h>                 /* calloc / free。 */
#include <string.h>                 /* memset。 */

static int frame_queue_should_stop(const struct frame_queue *queue)
{
    if (queue->stopped)             /* 队列自身已经停止。 */
        return 1;                   /* 调用者应该退出。 */
    if (queue->stop_flag != NULL && *queue->stop_flag) /* 外部 Ctrl+C/退出标志已置位。 */
        return 1;                   /* 调用者应该退出。 */
    return 0;                       /* 仍可继续工作。 */
}

void frame_packet_add_ref(struct frame_packet *packet) /* 每个新增消费者应增加一次引用；此实现保留作共享 V4L2 帧接口。 */
{
    __sync_add_and_fetch(&packet->refs, 1); /* GCC 原子加一，适配当前交叉工具链。 */
}

void frame_packet_release(struct frame_packet *packet) /* 最后一个引用才 QBUF；摄像头上下文必须存活到所有引用释放。 */
{
    if (packet == NULL)             /* 空指针不用处理。 */
        return;                     /* 直接返回。 */
    if (__sync_sub_and_fetch(&packet->refs, 1) != 0) /* 还有其他线程在使用。 */
        return;                     /* 不能释放。 */
    if (packet->cap != NULL)        /* 如果来自 V4L2 mmap buffer。 */
        video_capture_put_frame(packet->cap, &packet->frame); /* 最后一个使用者归还缓冲。 */
    free(packet);                   /* 释放帧结构本身。 */
}

struct frame_packet *frame_packet_from_v4l2(struct video_capture *cap,
                                            const struct video_frame *frame)
{
    struct frame_packet *packet;    /* 新帧结构。 */

    packet = calloc(1, sizeof(*packet)); /* 分配并清零帧结构。 */
    if (packet == NULL)             /* 分配失败。 */
        return NULL;                /* 返回空表示失败。 */

    packet->cap = cap;              /* 保存摄像头上下文，最后归还 buffer 要用。 */
    packet->frame = *frame;         /* 保存 V4L2 buffer 索引等信息。 */
    packet->data = frame->data;     /* 直接引用 mmap buffer，不做 MJPG memcpy。 */
    packet->size = frame->size;     /* 保存帧长度。 */
    packet->sequence = frame->sequence; /* 保存帧序号。 */
    packet->refs = 1;               /* 创建者持有一个本地引用。 */
    return packet;                  /* 返回可跨线程使用的帧。 */
}

int frame_queue_init(struct frame_queue *queue,
                     unsigned int capacity,
                     volatile sig_atomic_t *stop_flag)
{
    memset(queue, 0, sizeof(*queue)); /* 清空队列状态。 */
    if (capacity == 0U || capacity > FRAME_QUEUE_MAX_CAPACITY) /* 检查容量。 */
        return -1;                  /* 容量非法。 */

    queue->capacity = capacity;     /* 设置队列容量。 */
    queue->stop_flag = stop_flag;   /* 保存外部退出标志。 */
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) /* 初始化互斥锁。 */
        return -1;                  /* 初始化失败。 */
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) { /* 初始化非空条件。 */
        pthread_mutex_destroy(&queue->mutex); /* 释放已经初始化的锁。 */
        return -1;                  /* 初始化失败。 */
    }
    if (pthread_cond_init(&queue->not_full, NULL) != 0) { /* 初始化非满条件。 */
        pthread_cond_destroy(&queue->not_empty); /* 释放已经初始化的非空条件。 */
        pthread_mutex_destroy(&queue->mutex); /* 释放已经初始化的锁。 */
        return -1;                  /* 初始化失败。 */
    }
    return 0;                       /* 初始化成功。 */
}

void frame_queue_stop(struct frame_queue *queue)
{
    pthread_mutex_lock(&queue->mutex); /* 加锁修改停止标志。 */
    queue->stopped = 1;             /* 标记队列停止。 */
    pthread_cond_broadcast(&queue->not_empty); /* 唤醒等待取帧的线程。 */
    pthread_cond_broadcast(&queue->not_full); /* 唤醒等待放帧的线程。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁。 */
}

void frame_queue_destroy(struct frame_queue *queue)
{
    unsigned int i;                 /* 循环计数。 */

    pthread_mutex_lock(&queue->mutex); /* 加锁处理残留帧。 */
    for (i = 0; i < queue->count; i++) { /* 遍历残留帧。 */
        unsigned int pos = (queue->head + i) % queue->capacity; /* 计算槽位。 */

        frame_packet_release(queue->items[pos]); /* 释放残留帧引用。 */
        queue->items[pos] = NULL;  /* 清空槽位。 */
    }
    queue->head = 0;                /* 清空读位置。 */
    queue->tail = 0;                /* 清空写位置。 */
    queue->count = 0;               /* 清空数量。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁。 */

    pthread_cond_destroy(&queue->not_full); /* 销毁非满条件变量。 */
    pthread_cond_destroy(&queue->not_empty); /* 销毁非空条件变量。 */
    pthread_mutex_destroy(&queue->mutex); /* 销毁互斥锁。 */
}

int frame_queue_push_wait(struct frame_queue *queue, struct frame_packet *packet)
{
    pthread_mutex_lock(&queue->mutex); /* 加锁访问队列。 */
    while (!frame_queue_should_stop(queue) && queue->count == queue->capacity) /* 循环检查满队列谓词，兼容虚假唤醒与停止广播。 */
        pthread_cond_wait(&queue->not_full, &queue->mutex); /* 满队列等待。 */

    if (frame_queue_should_stop(queue)) { /* 停止时不再接收新帧。 */
        pthread_mutex_unlock(&queue->mutex); /* 解锁。 */
        return -1;                  /* 告诉调用者入队失败。 */
    }

    queue->items[queue->tail] = packet; /* 写入尾部槽位。 */
    queue->tail = (queue->tail + 1U) % queue->capacity; /* 尾指针前进。 */
    queue->count++;                 /* 队列帧数加一。 */
    pthread_cond_signal(&queue->not_empty); /* 唤醒消费者。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁。 */
    return 0;                       /* 入队成功。 */
}

int frame_queue_push_drop_oldest(struct frame_queue *queue,
                                 struct frame_packet *packet,
                                 unsigned int *dropped)
{
    pthread_mutex_lock(&queue->mutex); /* 加锁访问队列。 */
    if (frame_queue_should_stop(queue)) { /* 停止时不再接收新帧。 */
        pthread_mutex_unlock(&queue->mutex); /* 解锁。 */
        return -1;                  /* 入队失败。 */
    }

    if (queue->count == queue->capacity) { /* 预览队列已满。 */
        frame_packet_release(queue->items[queue->head]); /* 丢弃最旧帧引用。 */
        queue->items[queue->head] = NULL; /* 清空旧槽位。 */
        queue->head = (queue->head + 1U) % queue->capacity; /* 读指针前进。 */
        queue->count--;             /* 帧数减一。 */
        if (dropped != NULL)        /* 调用者需要统计丢帧。 */
            (*dropped)++;           /* 预览丢帧计数加一。 */
    }

    queue->items[queue->tail] = packet; /* 写入新帧。 */
    queue->tail = (queue->tail + 1U) % queue->capacity; /* 尾指针前进。 */
    queue->count++;                 /* 队列帧数加一。 */
    pthread_cond_signal(&queue->not_empty); /* 唤醒预览线程。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁。 */
    return 0;                       /* 入队成功。 */
}

struct frame_packet *frame_queue_pop(struct frame_queue *queue)
{
    struct frame_packet *packet;    /* 取出的帧。 */

    pthread_mutex_lock(&queue->mutex); /* 加锁访问队列。 */
    while (!queue->stopped && queue->count == 0) /* 空队列阻塞等数据；stop 必须广播条件变量才能及时唤醒。 */
        pthread_cond_wait(&queue->not_empty, &queue->mutex); /* 队空时等待新帧。 */

    if (queue->count == 0) {        /* 停止后队列已经取空。 */
        pthread_mutex_unlock(&queue->mutex); /* 解锁。 */
        return NULL;                /* 返回空表示线程可退出。 */
    }

    packet = queue->items[queue->head]; /* 取出头部帧。 */
    queue->items[queue->head] = NULL; /* 清空槽位。 */
    queue->head = (queue->head + 1U) % queue->capacity; /* 头指针前进。 */
    queue->count--;                 /* 队列帧数减一。 */
    pthread_cond_signal(&queue->not_full); /* 唤醒可能等待入队的采集线程。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁。 */
    return packet;                  /* 返回取出的帧。 */
}
