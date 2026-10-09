#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include "video_capture.h"          /* struct video_capture / struct video_frame。 */

#include <pthread.h>                /* pthread_mutex_t / pthread_cond_t。 */
#include <signal.h>                 /* sig_atomic_t。 */
#include <stddef.h>                 /* size_t。 */

#define FRAME_QUEUE_MAX_CAPACITY 8  /* 保存队列最大深度；预览队列可使用更小 capacity。 */

struct frame_packet {               /* 线程之间共享的一帧 MJPG 数据。 */
    struct video_capture *cap;      /* 持有这帧的 V4L2 摄像头上下文。 */
    struct video_frame frame;       /* 原始 V4L2 mmap 帧描述。 */
    unsigned char *data;            /* 指向 V4L2 mmap buffer 中的 MJPG 数据。 */
    size_t size;                    /* MJPG 数据长度。 */
    unsigned int sequence;          /* 摄像头帧序号，便于调试观察。 */
    volatile int refs;              /* 引用计数：保存队列和预览队列共享同一帧。 */
};

struct frame_queue {                /* MJPG 帧在线程之间流动的有界环形队列。 */
    struct frame_packet *items[FRAME_QUEUE_MAX_CAPACITY]; /* 队列槽位。 */
    unsigned int capacity;          /* 当前实际队列容量。 */
    unsigned int head;              /* 消费者读取位置。 */
    unsigned int tail;              /* 生产者写入位置。 */
    unsigned int count;             /* 当前队列内帧数量。 */
    int stopped;                    /* 队列停止标志。 */
    volatile sig_atomic_t *stop_flag; /* 外部 Ctrl+C/退出标志。 */
    pthread_mutex_t mutex;          /* 队列互斥锁。 */
    pthread_cond_t not_empty;       /* 队列非空条件变量。 */
    pthread_cond_t not_full;        /* 队列非满条件变量。 */
};

void frame_packet_add_ref(struct frame_packet *packet); /* 增加帧引用计数。 */
void frame_packet_release(struct frame_packet *packet); /* 减少引用；最后一个引用归还 V4L2 buffer。 */
struct frame_packet *frame_packet_from_v4l2(struct video_capture *cap,
                                            const struct video_frame *frame); /* 引用 V4L2 mmap 帧。 */

int frame_queue_init(struct frame_queue *queue,
                     unsigned int capacity,
                     volatile sig_atomic_t *stop_flag); /* 初始化帧队列。 */
void frame_queue_stop(struct frame_queue *queue); /* 停止队列并唤醒等待线程。 */
void frame_queue_destroy(struct frame_queue *queue); /* 释放残留帧并销毁同步原语。 */
int frame_queue_push_wait(struct frame_queue *queue,
                          struct frame_packet *packet); /* 满队列时等待，用于录制保存。 */
int frame_queue_push_drop_oldest(struct frame_queue *queue,
                                 struct frame_packet *packet,
                                 unsigned int *dropped); /* 满队列时丢旧帧，用于实时预览。 */
struct frame_packet *frame_queue_pop(struct frame_queue *queue); /* 阻塞取出一帧。 */

#endif
