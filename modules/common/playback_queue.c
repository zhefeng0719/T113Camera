#include "playback_queue.h"         /* playback_packet / playback_packet_queue。 */

#include <stdlib.h>                 /* calloc / malloc / free。 */
#include <string.h>                 /* memset / memcpy。 */

/* 释放一个播放 packet。 */
void playback_packet_free(struct playback_packet *packet)
{
    if (packet == NULL)             /* 允许空指针。 */
        return;                     /* 空指针直接返回。 */
    free(packet->data);             /* 释放 MJPEG 数据副本。 */
    free(packet);                   /* 释放 packet 结构体。 */
}

/* 创建一个播放 packet，并复制 MJPEG 数据。 */
struct playback_packet *playback_packet_create(const unsigned char *data,
                                               size_t size,
                                               int64_t pts_ms,
                                               int64_t duration_ms,
                                               unsigned int index)
{
    struct playback_packet *packet; /* 新 packet。 */

    packet = calloc(1, sizeof(*packet)); /* 分配并清零 packet。 */
    if (packet == NULL)             /* 分配失败。 */
        return NULL;                /* 返回空指针。 */

    packet->data = malloc(size);    /* 给 MJPEG 数据分配副本空间。 */
    if (packet->data == NULL) {     /* 数据缓冲分配失败。 */
        free(packet);               /* 释放已经分配的 packet。 */
        return NULL;                /* 返回空指针。 */
    }

    memcpy(packet->data, data, size); /* 复制一帧 MJPEG 数据。 */
    packet->size = size;            /* 保存数据长度。 */
    packet->pts_ms = pts_ms;        /* 保存容器 PTS。 */
    packet->duration_ms = duration_ms; /* 保存容器帧时长。 */
    packet->index = index;          /* 保存帧序号。 */
    return packet;                  /* 返回构造完成的 packet。 */
}

/* 初始化播放 packet 队列。 */
int playback_queue_init(struct playback_packet_queue *queue)
{
    memset(queue, 0, sizeof(*queue)); /* 清空队列状态。 */
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) /* 初始化互斥锁。 */
        return -1;                  /* 初始化失败。 */
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) { /* 初始化非空条件。 */
        pthread_mutex_destroy(&queue->mutex); /* 释放已经初始化的互斥锁。 */
        return -1;                  /* 初始化失败。 */
    }
    if (pthread_cond_init(&queue->not_full, NULL) != 0) { /* 初始化非满条件。 */
        pthread_cond_destroy(&queue->not_empty); /* 释放已经初始化的非空条件。 */
        pthread_mutex_destroy(&queue->mutex); /* 释放已经初始化的互斥锁。 */
        return -1;                  /* 初始化失败。 */
    }
    return 0;                       /* 初始化成功。 */
}

/* 清理播放 packet 队列。 */
void playback_queue_destroy(struct playback_packet_queue *queue)
{
    struct playback_packet *packet; /* 待释放 packet。 */

    pthread_mutex_lock(&queue->mutex); /* 加锁取出残留数据。 */
    while (queue->count > 0) {      /* 如果还有未消费 packet。 */
        packet = queue->items[queue->head]; /* 取出队头 packet。 */
        queue->head = (queue->head + 1U) % PLAYBACK_QUEUE_CAPACITY; /* 队头后移。 */
        queue->count--;             /* 队列数量减一。 */
        playback_packet_free(packet); /* 释放残留 packet。 */
    }
    pthread_mutex_unlock(&queue->mutex); /* 解锁队列。 */

    pthread_cond_destroy(&queue->not_full); /* 销毁非满条件变量。 */
    pthread_cond_destroy(&queue->not_empty); /* 销毁非空条件变量。 */
    pthread_mutex_destroy(&queue->mutex); /* 销毁互斥锁。 */
}

/* 停止播放队列，唤醒所有等待线程。 */
void playback_queue_stop(struct playback_packet_queue *queue)
{
    pthread_mutex_lock(&queue->mutex); /* 加锁修改队列状态。 */
    queue->stopped = 1;             /* 标记队列已经停止。 */
    pthread_cond_broadcast(&queue->not_empty); /* 唤醒等待数据的消费者。 */
    pthread_cond_broadcast(&queue->not_full); /* 唤醒等待空位的生产者。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁队列。 */
}

/* 标记播放队列已经生产结束。 */
void playback_queue_finish(struct playback_packet_queue *queue)
{
    pthread_mutex_lock(&queue->mutex); /* 加锁修改队列状态。 */
    queue->finished = 1;            /* 标记生产者已正常结束。 */
    pthread_cond_broadcast(&queue->not_empty); /* 唤醒可能正在等待的消费者。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁队列。 */
}

/* 向播放队列写入一个 packet。 */
int playback_queue_push(struct playback_packet_queue *queue,
                        struct playback_packet *packet)
{
    pthread_mutex_lock(&queue->mutex); /* 加锁保护环形队列。 */
    while (!queue->stopped && queue->count == PLAYBACK_QUEUE_CAPACITY)
        pthread_cond_wait(&queue->not_full, &queue->mutex); /* 队满时等待空位。 */

    if (queue->stopped) {           /* 队列已经停止。 */
        pthread_mutex_unlock(&queue->mutex); /* 先解锁。 */
        return -1;                  /* 通知生产者退出。 */
    }

    queue->items[queue->tail] = packet; /* 把 packet 写入队尾。 */
    queue->tail = (queue->tail + 1U) % PLAYBACK_QUEUE_CAPACITY; /* 队尾后移。 */
    queue->count++;                 /* 队列数量加一。 */
    pthread_cond_signal(&queue->not_empty); /* 通知消费者有数据。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁队列。 */
    return 0;                       /* 入队成功。 */
}

/* 从播放队列读出一个 packet；返回 0 表示结束，1 表示拿到 packet。 */
int playback_queue_pop(struct playback_packet_queue *queue,
                       struct playback_packet **packet)
{
    pthread_mutex_lock(&queue->mutex); /* 加锁保护环形队列。 */
    while (!queue->stopped && !queue->finished && queue->count == 0)
        pthread_cond_wait(&queue->not_empty, &queue->mutex); /* 队空时等待数据。 */

    if (queue->count == 0) {        /* 队列没有可消费数据。 */
        pthread_mutex_unlock(&queue->mutex); /* 先解锁。 */
        *packet = NULL;             /* 输出空 packet。 */
        return 0;                   /* 表示停止或正常结束。 */
    }

    *packet = queue->items[queue->head]; /* 取出队头 packet。 */
    queue->head = (queue->head + 1U) % PLAYBACK_QUEUE_CAPACITY; /* 队头后移。 */
    queue->count--;                 /* 队列数量减一。 */
    pthread_cond_signal(&queue->not_full); /* 通知生产者有空位。 */
    pthread_mutex_unlock(&queue->mutex); /* 解锁队列。 */
    return 1;                       /* 成功取到 packet。 */
}
