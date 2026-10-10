#include "wav_player.h"

#include "audio_alsa.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAV_READ_CHUNK 4096U

static uint16_t read_le16(const unsigned char *p) /* 按字节解析小端值，不依赖 CPU 对齐或本机端序。 */
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const unsigned char *p) /* RIFF 长度字段为小端 32 位整数。 */
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int wav_player_probe(const char *path, struct wav_player *info) /* 遍历 RIFF chunk，不能假设 WAV 头固定为 44 字节。 */
{
    FILE *fp;
    unsigned char header[12];
    int have_fmt = 0;
    int have_data = 0;
    uint16_t audio_format = 0;

    if (path == NULL || info == NULL)
        return -1;

    memset(info, 0, sizeof(*info));
    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return -1;
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header))
        goto fail;
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) /* 只接受 RIFF/WAVE，拒绝其他文件容器。 */
        goto fail;

    while (!have_data) { /* 逐块查找 fmt 和 data；未知块由 fseek 跳过。 */
        unsigned char chunk[8];
        uint32_t chunk_size;
        long chunk_data_pos;

        if (fread(chunk, 1, sizeof(chunk), fp) != sizeof(chunk))
            break;
        chunk_size = read_le32(chunk + 4); /* chunk 长度不含八字节标识与长度头。 */
        chunk_data_pos = ftell(fp); /* 保存块数据起点，后续定位不依赖已读取了多少字段。 */

        if (memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char fmt[16];

            if (chunk_size < sizeof(fmt))
                goto fail;
            if (fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt))
                goto fail;
            audio_format = read_le16(fmt + 0); /* 格式值 1 表示无压缩整数 PCM。 */
            info->channels = read_le16(fmt + 2); /* 声道数决定每帧字节数。 */
            info->sample_rate = read_le32(fmt + 4); /* 采样率决定播放速度，必须与文件一致。 */
            info->bits_per_sample = read_le16(fmt + 14); /* 此简化播放路径只支持 16 位 PCM。 */
            have_fmt = 1;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_offset = (uint32_t)chunk_data_pos; /* 保存音频负载位置，播放时跳过容器头。 */
            info->data_size = chunk_size; /* 音频负载字节数不等于整个文件长度。 */
            have_data = 1;
        }

        if (fseek(fp, chunk_data_pos + (long)chunk_size + (long)(chunk_size & 1U), /* RIFF 奇数长度块后有一个填充字节，不属于样本数据。 */
                  SEEK_SET) < 0)
            goto fail;
    }

    fclose(fp); /* 释放只读文件句柄。 */
    if (!have_fmt || !have_data || audio_format != 1 ||
        info->channels == 0 || info->sample_rate == 0 ||
        info->bits_per_sample != 16)
        return -1;
    return 0;

fail:
    fclose(fp); /* 释放只读文件句柄。 */
    return -1;
}

int wav_player_play_pcm16(const char *path,
                          const char *device,
                          volatile int *stop,
                          volatile int *paused,
                          void (*progress_cb)(uint32_t played_ms,
                                              uint32_t total_ms,
                                              void *user),
                          void *user)
{
    struct wav_player info;
    struct audio_pcm pcm;
    FILE *fp = NULL;
    unsigned char *buffer = NULL;
    size_t frame_bytes;
    uint32_t played_bytes = 0;
    uint32_t total_ms;
    int ret = -1;

    memset(&pcm, 0, sizeof(pcm));
    if (wav_player_probe(path, &info) < 0) {
        fprintf(stderr, "unsupported wav: %s\n", path);
        return -1;
    }

    frame_bytes = (size_t)info.channels * (info.bits_per_sample / 8U); /* 交错 PCM 一帧包含每个声道一个样本。 */
    if (frame_bytes == 0)
        return -1;
    total_ms = (uint32_t)(((uint64_t)info.data_size * 1000ULL) / /* 负载字节数换算总时长，先转 64 位避免乘法溢出。 */
                          ((uint64_t)info.sample_rate * frame_bytes));

    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return -1;
    }
    if (fseek(fp, (long)info.data_offset, SEEK_SET) < 0) /* 定位到 data 块首字节。 */
        goto out;

    buffer = malloc(WAV_READ_CHUNK);
    if (buffer == NULL) {
        perror("malloc wav buffer");
        goto out;
    }

    if (audio_playback_open(&pcm, device, info.sample_rate, info.channels, 1024) < 0) /* 按文件采样率和声道数打开播放设备。 */
        goto out;

    while ((stop == NULL || !*stop) && played_bytes < info.data_size) { /* 仅播放声明的 PCM 负载，不读尾部其他 chunk。 */
        size_t want;
        size_t got;
        snd_pcm_uframes_t frames;

        while (paused != NULL && *paused && (stop == NULL || !*stop)) /* 暂停停止提交新样本，停止请求仍能终止等待。 */
            usleep(10000);
        if (stop != NULL && *stop)
            break;

        want = info.data_size - played_bytes;
        if (want > WAV_READ_CHUNK)
            want = WAV_READ_CHUNK;
        want -= want % frame_bytes; /* 请求长度对齐完整 PCM 帧，避免拆开交错样本。 */
        if (want == 0)
            break;

        got = fread(buffer, 1, want, fp);
        if (got == 0) {
            if (ferror(fp))
                perror("fread wav");
            break;
        }
        got -= got % frame_bytes; /* 短读后也只播放完整帧。 */
        frames = (snd_pcm_uframes_t)(got / frame_bytes); /* ALSA writei 参数以帧为单位。 */
        if (audio_playback_write(&pcm, buffer, frames, frame_bytes) < 0)
            goto out;

        played_bytes += (uint32_t)got; /* 累计已提交字节数，此旧路径进度不是硬件消费时钟。 */
        if (progress_cb != NULL) {
            uint32_t played_ms = (uint32_t)(((uint64_t)played_bytes * 1000ULL) /
                                            ((uint64_t)info.sample_rate * frame_bytes));
            progress_cb(played_ms, total_ms, user); /* 报告读取并提交的进度；主项目同步路径由 FFmpeg 播放器负责。 */
        }
    }

    audio_playback_drain(&pcm); /* 正常结束等待声卡缓冲中剩余样本播完。 */
    ret = 0;

out:
    audio_pcm_close(&pcm); /* 所有退出路径统一关闭 PCM。 */
    free(buffer); /* 释放当前任务的临时数据缓冲；队列拥有的块另行清理。 */
    if (fp != NULL)
        fclose(fp); /* 释放只读文件句柄。 */
    return ret;
}
