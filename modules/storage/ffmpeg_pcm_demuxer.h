#ifndef FFMPEG_PCM_DEMUXER_H
#define FFMPEG_PCM_DEMUXER_H

#include <stddef.h>
#include <stdint.h>

#include <libavformat/avformat.h>

struct ffmpeg_pcm_demuxer {
    AVFormatContext *fmt;
    int audio_stream;
    AVRational time_base;
    AVPacket packet;
    int packet_valid;
    int64_t duration_ms;
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int bits_per_sample;
};

int ffmpeg_pcm_demuxer_open(struct ffmpeg_pcm_demuxer *demuxer,
                            const char *path);
int ffmpeg_pcm_demuxer_read(struct ffmpeg_pcm_demuxer *demuxer,
                            const unsigned char **data,
                            size_t *size,
                            int64_t *pts_ms);
void ffmpeg_pcm_demuxer_close(struct ffmpeg_pcm_demuxer *demuxer);

#endif
