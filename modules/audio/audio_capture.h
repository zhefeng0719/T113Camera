#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define AUDIO_CAPTURE_DEVICE "hw:0,0"
#define AUDIO_CAPTURE_RATE 48000
#define AUDIO_CAPTURE_CHANNELS 1
#define AUDIO_CAPTURE_PERIOD_FRAMES 1024
#define AUDIO_CAPTURE_BITS_PER_SAMPLE 16

struct audio_block {
    int64_t pts_us;
    unsigned int frames;
    unsigned int rate;
    unsigned int channels;
    unsigned char *data;
    size_t bytes;
};

struct audio_queue {
    struct audio_block *items;
    unsigned int capacity;
    unsigned int head;
    unsigned int tail;
    unsigned int count;
    int stopped;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
};

struct audio_capture_worker {
    struct audio_queue *queue;
    const char *device;
    unsigned int rate;
    unsigned int channels;
    unsigned int period_frames;
    unsigned int captured_blocks;
    unsigned int captured_frames;
    int ret;
};

int audio_queue_init(struct audio_queue *queue, unsigned int capacity);
void audio_queue_stop(struct audio_queue *queue);
void audio_queue_destroy(struct audio_queue *queue);
int audio_queue_push(struct audio_queue *queue, const struct audio_block *block);
int audio_queue_pop(struct audio_queue *queue, struct audio_block *block);
int audio_queue_try_pop(struct audio_queue *queue, struct audio_block *block);
void audio_block_free(struct audio_block *block);
void *audio_capture_thread_main(void *arg);

#endif
