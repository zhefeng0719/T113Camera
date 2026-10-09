#ifndef PLAYBACK_QUEUE_H
#define PLAYBACK_QUEUE_H

#include <pthread.h>               /* pthread_mutex_t / pthread_cond_t。 */
#include <stdint.h>                 /* int64_t。 */
#include <stddef.h>                 /* size_t。 */

#define PLAYBACK_QUEUE_CAPACITY 4   /* 播放队列深度：读线程最多领先显示线程 4 帧。 */

struct playback_packet {            /* 播放线程之间传递的一帧 MJPEG packet。 */
    unsigned char *data;            /* 复制出来的 MJPEG 数据。 */
    size_t size;                    /* MJPEG 数据长度。 */
    int64_t pts_ms;                 /* 容器时间戳，裸流没有时间戳时为 -1。 */
    int64_t duration_ms;            /* 容器帧时长，缺失时为 -1。 */
    unsigned int index;             /* 帧序号，裸流按它换算显示时间。 */
};

struct playback_packet_queue {      /* 读线程和显示线程之间的有界环形队列。 */
    struct playback_packet *items[PLAYBACK_QUEUE_CAPACITY]; /* 环形队列槽位。 */
    unsigned int head;              /* 消费者读取位置。 */
    unsigned int tail;              /* 生产者写入位置。 */
    unsigned int count;             /* 当前队列内 packet 数量。 */
    int stopped;                    /* 用户打断或出错时置 1。 */
    int finished;                   /* 生产者读到文件结束时置 1。 */
    pthread_mutex_t mutex;          /* 队列状态保护锁。 */
    pthread_cond_t not_empty;       /* 队列非空条件变量。 */
    pthread_cond_t not_full;        /* 队列非满条件变量。 */
};

struct playback_packet *playback_packet_create(const unsigned char *data,
                                               size_t size,
                                               int64_t pts_ms,
                                               int64_t duration_ms,
                                               unsigned int index); /* 复制一帧 MJPEG 并生成 packet。 */
void playback_packet_free(struct playback_packet *packet); /* 释放 packet 和内部数据。 */
int playback_queue_init(struct playback_packet_queue *queue); /* 初始化队列和同步原语。 */
void playback_queue_destroy(struct playback_packet_queue *queue); /* 释放残留 packet 并销毁同步原语。 */
void playback_queue_stop(struct playback_packet_queue *queue); /* 停止队列并唤醒等待线程。 */
void playback_queue_finish(struct playback_packet_queue *queue); /* 标记生产者已正常结束。 */
int playback_queue_push(struct playback_packet_queue *queue,
                        struct playback_packet *packet); /* 生产者阻塞式写入 packet。 */
int playback_queue_pop(struct playback_packet_queue *queue,
                       struct playback_packet **packet); /* 消费者阻塞式读取 packet。 */

#endif
