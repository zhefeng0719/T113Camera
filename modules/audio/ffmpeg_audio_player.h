#ifndef FFMPEG_AUDIO_PLAYER_H
#define FFMPEG_AUDIO_PLAYER_H

#include <stdint.h>
#include "media_clock.h"

struct ffmpeg_audio_info {
    unsigned int sample_rate; /* 音频采样率，单位 Hz。 */
    unsigned int channels; /* 每个音频帧中的声道数量。 */
    int64_t duration_ms; /* 媒体总时长，未知时为负值。 */
};

int ffmpeg_audio_probe(const char *path, struct ffmpeg_audio_info *info);
int ffmpeg_audio_probe_interruptible(const char *path, struct ffmpeg_audio_info *info,
                                      volatile int *stop); /* 探测期间也接受会话取消。 */
int ffmpeg_audio_play_clock(const char *path, const char *device,
                            volatile int *stop, volatile int *paused,
                            void (*progress_cb)(uint32_t, uint32_t, void *),
                            void *user, struct media_clock *clock);
int ffmpeg_audio_play(const char *path,
                      const char *device,
                      volatile int *stop,
                      volatile int *paused,
                      void (*progress_cb)(uint32_t played_ms,
                                          uint32_t total_ms,
                                          void *user),
                      void *user);

#endif
