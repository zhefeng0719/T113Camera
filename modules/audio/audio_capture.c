/*
 * audio_capture.c
 *
 * T113Camera 音频采集模块。
 *
 * 这一层只负责把 ALSA capture 数据变成带时间戳的 PCM block。
 * 它不写 WAV，不编码 AAC，也不做容器封装。
 */

#include "audio_capture.h"
#include "audio_alsa.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static size_t audio_frame_bytes(unsigned int channels)
{
    return channels * AUDIO_CAPTURE_BITS_PER_SAMPLE / 8;
}

static int64_t now_monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

int audio_queue_init(struct audio_queue *queue, unsigned int capacity)
{
    memset(queue, 0, sizeof(*queue));
    queue->items = calloc(capacity, sizeof(queue->items[0]));
    if (queue->items == NULL)
        return -1;
    queue->capacity = capacity;
    if (pthread_mutex_init(&queue->mutex, NULL) != 0)
        goto fail;
    if (pthread_cond_init(&queue->not_empty, NULL) != 0)
        goto fail_mutex;
    if (pthread_cond_init(&queue->not_full, NULL) != 0)
        goto fail_not_empty;
    return 0;

fail_not_empty:
    pthread_cond_destroy(&queue->not_empty);
fail_mutex:
    pthread_mutex_destroy(&queue->mutex);
fail:
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
    return -1;
}

void audio_queue_stop(struct audio_queue *queue)
{
    pthread_mutex_lock(&queue->mutex);
    queue->stopped = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

void audio_block_free(struct audio_block *block)
{
    if (block == NULL)
        return;
    free(block->data);
    memset(block, 0, sizeof(*block));
}

void audio_queue_destroy(struct audio_queue *queue)
{
    unsigned int i;

    if (queue == NULL || queue->items == NULL)
        return;
    for (i = 0; i < queue->count; i++) {
        unsigned int pos = (queue->head + i) % queue->capacity;
        audio_block_free(&queue->items[pos]);
    }
    pthread_cond_destroy(&queue->not_full);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

int audio_queue_push(struct audio_queue *queue, const struct audio_block *block)
{
    pthread_mutex_lock(&queue->mutex);
    while (!queue->stopped && queue->count == queue->capacity)
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    if (queue->stopped) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }
    queue->items[queue->tail] = *block;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

int audio_queue_pop(struct audio_queue *queue, struct audio_block *block)
{
    pthread_mutex_lock(&queue->mutex);
    while (!queue->stopped && queue->count == 0)
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    *block = queue->items[queue->head];
    memset(&queue->items[queue->head], 0, sizeof(queue->items[queue->head]));
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

int audio_queue_try_pop(struct audio_queue *queue, struct audio_block *block)
{
    pthread_mutex_lock(&queue->mutex);
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    *block = queue->items[queue->head];
    memset(&queue->items[queue->head], 0, sizeof(queue->items[queue->head]));
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

void *audio_capture_thread_main(void *arg)
{
    struct audio_capture_worker *worker = arg;
    struct audio_pcm capture;
    const char *device = worker->device ? worker->device : AUDIO_CAPTURE_DEVICE;
    unsigned int rate = worker->rate ? worker->rate : AUDIO_CAPTURE_RATE;
    unsigned int channels = worker->channels ? worker->channels : AUDIO_CAPTURE_CHANNELS;
    unsigned int period = worker->period_frames ? worker->period_frames : AUDIO_CAPTURE_PERIOD_FRAMES;
    size_t frame_bytes = audio_frame_bytes(channels);
    size_t buffer_bytes = period * frame_bytes;
    unsigned char *buffer = NULL;

    worker->ret = -1;
    if (audio_capture_open(&capture, device, rate, channels, period) < 0)
        return NULL;

    buffer = malloc(buffer_bytes);
    if (buffer == NULL) {
        perror("malloc audio capture buffer");
        goto out_capture;
    }

    while (!worker->queue->stopped) {
        struct audio_block block;
        snd_pcm_sframes_t got;
        size_t bytes;

        got = audio_capture_read(&capture, buffer, period);
        if (got < 0)
            goto out_buffer;
        if (got == 0)
            continue;

        bytes = (size_t)got * frame_bytes;
        memset(&block, 0, sizeof(block));
        block.data = malloc(bytes);
        if (block.data == NULL) {
            perror("malloc audio block");
            goto out_buffer;
        }
        memcpy(block.data, buffer, bytes);
        block.pts_us = now_monotonic_us();
        block.frames = (unsigned int)got;
        block.rate = capture.rate;
        block.channels = capture.channels;
        block.bytes = bytes;

        if (audio_queue_push(worker->queue, &block) < 0) {
            audio_block_free(&block);
            break;
        }

        worker->captured_blocks++;
        worker->captured_frames += (unsigned int)got;
    }

    worker->ret = 0;

out_buffer:
    free(buffer);
out_capture:
    audio_pcm_close(&capture);
    audio_queue_stop(worker->queue);
    return NULL;
}
