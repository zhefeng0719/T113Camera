#ifndef AUDIO_ALSA_H
#define AUDIO_ALSA_H

#include <alsa/asoundlib.h>         /* 使用 snd_pcm_t 等 ALSA 类型。 */
#include <stddef.h>                 /* 使用 size_t。 */

/* 一个已经打开的 ALSA PCM 设备。 */
struct audio_pcm {
    snd_pcm_t *handle;              /* ALSA PCM 句柄。 */
    unsigned int rate;              /* 实际使用的采样率。 */
    unsigned int channels;          /* 实际使用的声道数。 */
    snd_pcm_uframes_t period;       /* 每次建议读写的帧数。 */
};

/* 打开并配置录音设备。 */
int audio_capture_open(struct audio_pcm *pcm, const char *device,
                       unsigned int rate, unsigned int channels,
                       snd_pcm_uframes_t period);

/* 从录音设备读取 PCM 帧。 */
snd_pcm_sframes_t audio_capture_read(struct audio_pcm *pcm, void *buffer,
                                     snd_pcm_uframes_t frames);

/* 打开并配置播放设备。 */
int audio_playback_open(struct audio_pcm *pcm, const char *device,
                        unsigned int rate, unsigned int channels,
                        snd_pcm_uframes_t period);

/* 向播放设备写入 PCM 帧。 */
int audio_playback_write(struct audio_pcm *pcm, const void *buffer,
                         snd_pcm_uframes_t frames, size_t frame_bytes);

/* 等待播放缓冲播完。 */
void audio_playback_drain(struct audio_pcm *pcm);

/* 关闭 PCM 设备。 */
void audio_pcm_close(struct audio_pcm *pcm);

#endif
