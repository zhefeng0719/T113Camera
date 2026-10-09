#include "wav_player.h"

#include "audio_alsa.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAV_READ_CHUNK 4096U

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int wav_player_probe(const char *path, struct wav_player *info)
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
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0)
        goto fail;

    while (!have_data) {
        unsigned char chunk[8];
        uint32_t chunk_size;
        long chunk_data_pos;

        if (fread(chunk, 1, sizeof(chunk), fp) != sizeof(chunk))
            break;
        chunk_size = read_le32(chunk + 4);
        chunk_data_pos = ftell(fp);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            unsigned char fmt[16];

            if (chunk_size < sizeof(fmt))
                goto fail;
            if (fread(fmt, 1, sizeof(fmt), fp) != sizeof(fmt))
                goto fail;
            audio_format = read_le16(fmt + 0);
            info->channels = read_le16(fmt + 2);
            info->sample_rate = read_le32(fmt + 4);
            info->bits_per_sample = read_le16(fmt + 14);
            have_fmt = 1;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_offset = (uint32_t)chunk_data_pos;
            info->data_size = chunk_size;
            have_data = 1;
        }

        if (fseek(fp, chunk_data_pos + (long)chunk_size + (long)(chunk_size & 1U),
                  SEEK_SET) < 0)
            goto fail;
    }

    fclose(fp);
    if (!have_fmt || !have_data || audio_format != 1 ||
        info->channels == 0 || info->sample_rate == 0 ||
        info->bits_per_sample != 16)
        return -1;
    return 0;

fail:
    fclose(fp);
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

    frame_bytes = (size_t)info.channels * (info.bits_per_sample / 8U);
    if (frame_bytes == 0)
        return -1;
    total_ms = (uint32_t)(((uint64_t)info.data_size * 1000ULL) /
                          ((uint64_t)info.sample_rate * frame_bytes));

    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return -1;
    }
    if (fseek(fp, (long)info.data_offset, SEEK_SET) < 0)
        goto out;

    buffer = malloc(WAV_READ_CHUNK);
    if (buffer == NULL) {
        perror("malloc wav buffer");
        goto out;
    }

    if (audio_playback_open(&pcm, device, info.sample_rate, info.channels, 1024) < 0)
        goto out;

    while ((stop == NULL || !*stop) && played_bytes < info.data_size) {
        size_t want;
        size_t got;
        snd_pcm_uframes_t frames;

        while (paused != NULL && *paused && (stop == NULL || !*stop))
            usleep(10000);
        if (stop != NULL && *stop)
            break;

        want = info.data_size - played_bytes;
        if (want > WAV_READ_CHUNK)
            want = WAV_READ_CHUNK;
        want -= want % frame_bytes;
        if (want == 0)
            break;

        got = fread(buffer, 1, want, fp);
        if (got == 0) {
            if (ferror(fp))
                perror("fread wav");
            break;
        }
        got -= got % frame_bytes;
        frames = (snd_pcm_uframes_t)(got / frame_bytes);
        if (audio_playback_write(&pcm, buffer, frames, frame_bytes) < 0)
            goto out;

        played_bytes += (uint32_t)got;
        if (progress_cb != NULL) {
            uint32_t played_ms = (uint32_t)(((uint64_t)played_bytes * 1000ULL) /
                                            ((uint64_t)info.sample_rate * frame_bytes));
            progress_cb(played_ms, total_ms, user);
        }
    }

    audio_playback_drain(&pcm);
    ret = 0;

out:
    audio_pcm_close(&pcm);
    free(buffer);
    if (fp != NULL)
        fclose(fp);
    return ret;
}
