#ifndef WAV_PLAYER_H
#define WAV_PLAYER_H

#include <stdint.h>

struct wav_player {
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int bits_per_sample;
    uint32_t data_offset;
    uint32_t data_size;
};

int wav_player_probe(const char *path, struct wav_player *info);
int wav_player_play_pcm16(const char *path,
                          const char *device,
                          volatile int *stop,
                          volatile int *paused,
                          void (*progress_cb)(uint32_t played_ms,
                                              uint32_t total_ms,
                                              void *user),
                          void *user);

#endif
