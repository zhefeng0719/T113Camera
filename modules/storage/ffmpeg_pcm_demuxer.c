#include "ffmpeg_pcm_demuxer.h"

#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>

#include <stdio.h>
#include <string.h>

static int64_t rescale_to_ms(int64_t value, AVRational time_base)
{
    if (value == AV_NOPTS_VALUE)
        return -1;
    return av_rescale_q(value, time_base, (AVRational){1, 1000});
}

int ffmpeg_pcm_demuxer_open(struct ffmpeg_pcm_demuxer *demuxer,
                            const char *path)
{
    unsigned int i;

    if (demuxer == NULL || path == NULL)
        return -1;

    memset(demuxer, 0, sizeof(*demuxer));
    demuxer->audio_stream = -1;
    demuxer->duration_ms = -1;
    av_init_packet(&demuxer->packet);

    if (avformat_open_input(&demuxer->fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "ffmpeg audio: open input failed: %s\n", path);
        return -1;
    }
    if (avformat_find_stream_info(demuxer->fmt, NULL) < 0) {
        fprintf(stderr, "ffmpeg audio: find stream info failed\n");
        ffmpeg_pcm_demuxer_close(demuxer);
        return -1;
    }

    for (i = 0; i < demuxer->fmt->nb_streams; i++) {
        AVStream *stream = demuxer->fmt->streams[i];
        AVCodecParameters *par = stream->codecpar;

        if (par->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (par->codec_id != AV_CODEC_ID_PCM_S16LE)
            continue;
        if (par->sample_rate <= 0 || par->channels <= 0)
            continue;

        demuxer->audio_stream = (int)i;
        demuxer->time_base = stream->time_base;
        demuxer->sample_rate = (unsigned int)par->sample_rate;
        demuxer->channels = (unsigned int)par->channels;
        demuxer->bits_per_sample = 16;
        if (stream->duration != AV_NOPTS_VALUE)
            demuxer->duration_ms = rescale_to_ms(stream->duration,
                                                 stream->time_base);
        break;
    }

    if (demuxer->audio_stream < 0) {
        ffmpeg_pcm_demuxer_close(demuxer);
        return -1;
    }
    if (demuxer->duration_ms < 0 && demuxer->fmt->duration != AV_NOPTS_VALUE)
        demuxer->duration_ms = demuxer->fmt->duration / 1000;

    printf("ffmpeg audio: stream=%d %u Hz %u ch duration=%lldms\n",
           demuxer->audio_stream,
           demuxer->sample_rate,
           demuxer->channels,
           (long long)demuxer->duration_ms);
    return 0;
}

int ffmpeg_pcm_demuxer_read(struct ffmpeg_pcm_demuxer *demuxer,
                            const unsigned char **data,
                            size_t *size,
                            int64_t *pts_ms)
{
    int ret;

    if (data != NULL)
        *data = NULL;
    if (size != NULL)
        *size = 0;
    if (pts_ms != NULL)
        *pts_ms = -1;
    if (demuxer == NULL || demuxer->fmt == NULL)
        return -1;

    if (demuxer->packet_valid) {
        av_packet_unref(&demuxer->packet);
        demuxer->packet_valid = 0;
    }

    while ((ret = av_read_frame(demuxer->fmt, &demuxer->packet)) >= 0) {
        AVPacket *pkt = &demuxer->packet;

        if (pkt->stream_index != demuxer->audio_stream) {
            av_packet_unref(pkt);
            continue;
        }

        demuxer->packet_valid = 1;
        if (data != NULL)
            *data = pkt->data;
        if (size != NULL)
            *size = (size_t)pkt->size;
        if (pts_ms != NULL)
            *pts_ms = rescale_to_ms(pkt->pts, demuxer->time_base);
        return 1;
    }

    return 0;
}

void ffmpeg_pcm_demuxer_close(struct ffmpeg_pcm_demuxer *demuxer)
{
    if (demuxer == NULL)
        return;

    if (demuxer->packet_valid) {
        av_packet_unref(&demuxer->packet);
        demuxer->packet_valid = 0;
    }
    if (demuxer->fmt != NULL)
        avformat_close_input(&demuxer->fmt);
    memset(demuxer, 0, sizeof(*demuxer));
    demuxer->audio_stream = -1;
}
