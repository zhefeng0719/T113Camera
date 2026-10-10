#ifndef WAV_PLAYER_H
#define WAV_PLAYER_H

#include <stdint.h>

struct wav_player {
    unsigned int sample_rate; /* 音频采样率，单位 Hz。 */
    unsigned int channels; /* 每个音频帧中的声道数量。 */
    unsigned int bits_per_sample; /* PCM 每声道样本位数。 */
    uint32_t data_offset; /* WAV data 块在文件中的字节偏移。 */
    uint32_t data_size; /* PCM 负载字节数，不包括容器头。 */
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
