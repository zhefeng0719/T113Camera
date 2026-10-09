#ifndef FFMPEG_AUDIO_PLAYER_H
#define FFMPEG_AUDIO_PLAYER_H

#include <stdint.h>

struct ffmpeg_audio_info {
    unsigned int sample_rate;
    unsigned int channels;
    int64_t duration_ms;
};

int ffmpeg_audio_probe(const char *path, struct ffmpeg_audio_info *info);
int ffmpeg_audio_play(const char *path,
                      const char *device,
                      volatile int *stop,
                      volatile int *paused,
                      void (*progress_cb)(uint32_t played_ms,
                                          uint32_t total_ms,
                                          void *user),
                      void *user);

#endif
