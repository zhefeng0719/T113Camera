/*
 * audio_alsa.c
 *
 * ALSA 设备层。
 *
 * 这个文件只负责和 ALSA PCM 设备打交道：
 *   - 打开录音设备。
 *   - 打开播放设备。
 *   - 读取 PCM 帧。
 *   - 写入 PCM 帧。
 *   - 关闭设备。
 *
 * 它不关心 WAV 文件，也不关心耳机/喇叭路由。
 */

#include "audio_alsa.h"             /* 本模块的结构体和函数声明。 */

#include <stdio.h>                  /* printf / fprintf。 */
#include <string.h>                 /* memset。 */

/* 配置一个 PCM 设备的通用硬件参数。录音和播放都会用到。 */
static int setup_hw_params(struct audio_pcm *pcm,
                           snd_pcm_stream_t stream,
                           const char *device,
                           unsigned int rate,
                           unsigned int channels,
                           snd_pcm_uframes_t period)
{
    snd_pcm_hw_params_t *params;    /* ALSA 硬件参数对象。 */
    unsigned int actual_rate = rate;/* set_rate_near 可能会改成最接近的真实采样率。 */
    snd_pcm_uframes_t actual_period = period; /* period 也可能被驱动微调。 */
    int rc;                         /* ALSA 函数返回值。 */

    memset(pcm, 0, sizeof(*pcm));   /* 先清空，避免失败时残留旧句柄。 */

    rc = snd_pcm_open(&pcm->handle, device, stream, 0); /* 打开 hw:0,0。 */
    if (rc < 0) {
        fprintf(stderr, "snd_pcm_open %s failed: %s\n", device, snd_strerror(rc));
        return -1;
    }

    snd_pcm_hw_params_alloca(&params);       /* 在栈上分配 hw params。 */

    rc = snd_pcm_hw_params_any(pcm->handle, params); /* 初始化参数范围。 */
    if (rc < 0) {
        fprintf(stderr, "snd_pcm_hw_params_any failed: %s\n", snd_strerror(rc));
        goto fail;
    }

    rc = snd_pcm_hw_params_set_access(pcm->handle, params,
                                      SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {                  /* interleaved 表示多声道样本按帧交错排列。 */
        fprintf(stderr, "set access failed: %s\n", snd_strerror(rc));
        goto fail;
    }

    rc = snd_pcm_hw_params_set_format(pcm->handle, params,
                                      SND_PCM_FORMAT_S16_LE);
    if (rc < 0) {                  /* 当前项目统一使用 16-bit 小端 PCM。 */
        fprintf(stderr, "set S16_LE failed: %s\n", snd_strerror(rc));
        goto fail;
    }

    rc = snd_pcm_hw_params_set_channels(pcm->handle, params, channels);
    if (rc < 0) {                  /* 当前基准是 mono，所以 channels=1。 */
        fprintf(stderr, "set channels %u failed: %s\n", channels, snd_strerror(rc));
        goto fail;
    }

    rc = snd_pcm_hw_params_set_rate_near(pcm->handle, params,
                                         &actual_rate, NULL);
    if (rc < 0) {                  /* 请求 48000，驱动返回实际可用采样率。 */
        fprintf(stderr, "set rate %u failed: %s\n", rate, snd_strerror(rc));
        goto fail;
    }

    rc = snd_pcm_hw_params_set_period_size_near(pcm->handle, params,
                                                &actual_period, NULL);
    if (rc < 0) {                  /* period 决定一次读写多少帧。 */
        fprintf(stderr, "set period failed: %s\n", snd_strerror(rc));
        goto fail;
    }

    rc = snd_pcm_hw_params(pcm->handle, params);     /* 提交硬件参数。 */
    if (rc < 0) {
        fprintf(stderr, "snd_pcm_hw_params failed: %s\n", snd_strerror(rc));
        goto fail;
    }

    pcm->rate = actual_rate;        /* 保存驱动最终确认的采样率。 */
    pcm->channels = channels;       /* 保存声道数。 */
    pcm->period = actual_period;    /* 保存最终 period。 */
    return 0;

fail:
    audio_pcm_close(pcm);           /* 失败时统一关闭。 */
    return -1;
}

/* 打开录音设备。 */
int audio_capture_open(struct audio_pcm *pcm, const char *device,
                       unsigned int rate, unsigned int channels,
                       snd_pcm_uframes_t period)
{
    if (setup_hw_params(pcm, SND_PCM_STREAM_CAPTURE, device,
                        rate, channels, period) < 0) {
        return -1;
    }

    printf("capture device : %s\n", device);
    printf("capture format : PCM S16_LE, %u channel(s), %u Hz\n",
           pcm->channels, pcm->rate);
    printf("period frames  : %lu\n", (unsigned long)pcm->period);
    return 0;
}

/* 从录音设备读取一批 PCM 帧。 */
snd_pcm_sframes_t audio_capture_read(struct audio_pcm *pcm, void *buffer,
                                     snd_pcm_uframes_t frames)
{
    while (1) {                                 /* overrun 时会重试。 */
        snd_pcm_sframes_t got = snd_pcm_readi(pcm->handle, buffer, frames);

        if (got == -EPIPE) {                   /* 录音 overrun。 */
            fprintf(stderr, "capture overrun, prepare again\n");
            snd_pcm_prepare(pcm->handle);      /* 恢复设备状态。 */
            continue;
        }
        if (got < 0) {                         /* 其它错误直接返回。 */
            fprintf(stderr, "snd_pcm_readi failed: %s\n", snd_strerror((int)got));
        }
        return got;                            /* 正数表示读到的帧数。 */
    }
}

/* 打开播放设备。 */
int audio_playback_open(struct audio_pcm *pcm, const char *device,
                        unsigned int rate, unsigned int channels,
                        snd_pcm_uframes_t period)
{
    if (setup_hw_params(pcm, SND_PCM_STREAM_PLAYBACK, device,
                        rate, channels, period) < 0) {
        return -1;
    }

    printf("playback device: %s\n", device);
    printf("playback format: PCM S16_LE, %u channel(s), %u Hz\n",
           pcm->channels, pcm->rate);
    return 0;
}

/* 把指定帧数完整写入播放设备。 */
int audio_playback_write(struct audio_pcm *pcm, const void *buffer,
                         snd_pcm_uframes_t frames, size_t frame_bytes)
{
    const char *bytes = (const char *)buffer;  /* 按字节移动指针。 */
    snd_pcm_uframes_t offset = 0;              /* 已经写入多少帧。 */

    while (offset < frames) {
        snd_pcm_sframes_t written = snd_pcm_writei(
            pcm->handle,
            bytes + offset * frame_bytes,
            frames - offset);

        if (written == -EPIPE) {               /* 播放 underrun。 */
            fprintf(stderr, "playback underrun, prepare again\n");
            snd_pcm_prepare(pcm->handle);      /* 恢复设备状态。 */
            continue;
        }
        if (written < 0) {                     /* 其它错误直接失败。 */
            fprintf(stderr, "snd_pcm_writei failed: %s\n", snd_strerror((int)written));
            return -1;
        }
        if (written == 0) {                    /* 没写进去就继续尝试。 */
            continue;
        }

        offset += (snd_pcm_uframes_t)written;  /* 累加已写帧数。 */
    }

    return 0;
}

/* 等待播放缓冲区里的声音播完。 */
void audio_playback_drain(struct audio_pcm *pcm)
{
    if (pcm && pcm->handle) {                  /* 句柄有效才 drain。 */
        snd_pcm_drain(pcm->handle);
    }
}

/* 关闭 PCM 设备。 */
void audio_pcm_close(struct audio_pcm *pcm)
{
    if (!pcm) {                                /* 防御空指针。 */
        return;
    }
    if (pcm->handle) {                         /* 句柄存在才关闭。 */
        snd_pcm_close(pcm->handle);
    }
    memset(pcm, 0, sizeof(*pcm));              /* 清空结构体，避免误用旧句柄。 */
}
