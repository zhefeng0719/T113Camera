#include "ffmpeg_audio_player.h"

#include "audio_alsa.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AUDIO_OUT_PERIOD 1024U

static int64_t rescale_to_ms(int64_t value, AVRational time_base)
{
    if (value == AV_NOPTS_VALUE)
        return -1;
    return av_rescale_q(value, time_base, (AVRational){1, 1000});
}

static int find_audio_stream(AVFormatContext *fmt)
{
    unsigned int i;

    for (i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters *par = fmt->streams[i]->codecpar;

        if (par->codec_type == AVMEDIA_TYPE_AUDIO)
            return (int)i;
    }
    return -1;
}

int ffmpeg_audio_probe(const char *path, struct ffmpeg_audio_info *info)
{
    AVFormatContext *fmt = NULL;
    int stream_index;
    AVCodecParameters *par;
    int ret = -1;

    if (path == NULL || info == NULL)
        return -1;
    memset(info, 0, sizeof(*info));
    info->duration_ms = -1;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
        return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0)
        goto out;

    stream_index = find_audio_stream(fmt);
    if (stream_index < 0)
        goto out;

    par = fmt->streams[stream_index]->codecpar;
    info->sample_rate = (unsigned int)par->sample_rate;
    info->channels = (unsigned int)par->channels;
    if (fmt->streams[stream_index]->duration != AV_NOPTS_VALUE)
        info->duration_ms = rescale_to_ms(fmt->streams[stream_index]->duration,
                                          fmt->streams[stream_index]->time_base);
    if (info->duration_ms < 0 && fmt->duration != AV_NOPTS_VALUE)
        info->duration_ms = fmt->duration / 1000;
    ret = (info->sample_rate > 0 && info->channels > 0) ? 0 : -1;

out:
    avformat_close_input(&fmt);
    return ret;
}

int ffmpeg_audio_play(const char *path,
                      const char *device,
                      volatile int *stop,
                      volatile int *paused,
                      void (*progress_cb)(uint32_t played_ms,
                                          uint32_t total_ms,
                                          void *user),
                      void *user)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *codec = NULL;
    const AVCodec *decoder;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr = NULL;
    struct audio_pcm pcm;
    int stream_index;
    int64_t out_layout;
    int64_t in_layout;
    int out_channels;
    int out_rate;
    uint8_t *out_buf = NULL;
    int out_buf_size = 0;
    int64_t duration_ms = -1;
    int ret = -1;

    memset(&pcm, 0, sizeof(pcm));

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "ffmpeg audio: open failed: %s\n", path);
        return -1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0)
        goto out;

    stream_index = find_audio_stream(fmt);
    if (stream_index < 0)
        goto out;

    decoder = avcodec_find_decoder(fmt->streams[stream_index]->codecpar->codec_id);
    if (decoder == NULL) {
        fprintf(stderr, "ffmpeg audio: decoder missing\n");
        goto out;
    }

    codec = avcodec_alloc_context3(decoder);
    if (codec == NULL)
        goto out;
    if (avcodec_parameters_to_context(codec, fmt->streams[stream_index]->codecpar) < 0)
        goto out;
    if (avcodec_open2(codec, decoder, NULL) < 0)
        goto out;

    out_channels = codec->channels > 0 ? codec->channels : 1;
    if (out_channels > 2)
        out_channels = 2;
    out_rate = codec->sample_rate > 0 ? codec->sample_rate : 48000;
    out_layout = av_get_default_channel_layout(out_channels);
    in_layout = codec->channel_layout ? (int64_t)codec->channel_layout :
                av_get_default_channel_layout(codec->channels);
    if (duration_ms < 0 && fmt->streams[stream_index]->duration != AV_NOPTS_VALUE)
        duration_ms = rescale_to_ms(fmt->streams[stream_index]->duration,
                                    fmt->streams[stream_index]->time_base);
    if (duration_ms < 0 && fmt->duration != AV_NOPTS_VALUE)
        duration_ms = fmt->duration / 1000;

    swr = swr_alloc_set_opts(NULL,
                             out_layout,
                             AV_SAMPLE_FMT_S16,
                             out_rate,
                             in_layout,
                             codec->sample_fmt,
                             codec->sample_rate,
                             0,
                             NULL);
    if (swr == NULL || swr_init(swr) < 0)
        goto out;

    if (audio_playback_open(&pcm, device, (unsigned int)out_rate,
                            (unsigned int)out_channels,
                            AUDIO_OUT_PERIOD) < 0)
        goto out;

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == NULL || frame == NULL)
        goto out;

    printf("ffmpeg audio: %s %d Hz %d ch duration=%lldms\n",
           avcodec_get_name(codec->codec_id),
           out_rate,
           out_channels,
           (long long)duration_ms);

    while (stop == NULL || !*stop) {
        int read_ret;

        while (paused != NULL && *paused && (stop == NULL || !*stop))
            usleep(10000);
        if (stop != NULL && *stop)
            break;

        read_ret = av_read_frame(fmt, packet);
        if (read_ret < 0)
            break;
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(codec, packet) < 0) {
            av_packet_unref(packet);
            break;
        }
        av_packet_unref(packet);

        while (stop == NULL || !*stop) {
            int samples;
            int need;
            int converted;
            int64_t pts_ms;

            read_ret = avcodec_receive_frame(codec, frame);
            if (read_ret == AVERROR(EAGAIN) || read_ret == AVERROR_EOF)
                break;
            if (read_ret < 0)
                goto out;

            samples = av_rescale_rnd(swr_get_delay(swr, codec->sample_rate) +
                                     frame->nb_samples,
                                     out_rate,
                                     codec->sample_rate,
                                     AV_ROUND_UP);
            need = av_samples_get_buffer_size(NULL, out_channels, samples,
                                              AV_SAMPLE_FMT_S16, 1);
            if (need > out_buf_size) {
                uint8_t *new_buf = realloc(out_buf, (size_t)need);

                if (new_buf == NULL)
                    goto out;
                out_buf = new_buf;
                out_buf_size = need;
            }

            converted = swr_convert(swr,
                                    &out_buf,
                                    samples,
                                    (const uint8_t **)frame->extended_data,
                                    frame->nb_samples);
            if (converted < 0)
                goto out;
            if (converted > 0) {
                size_t frame_bytes = (size_t)out_channels * 2U;

                if (audio_playback_write(&pcm, out_buf,
                                         (snd_pcm_uframes_t)converted,
                                         frame_bytes) < 0)
                    goto out;
            }

            pts_ms = rescale_to_ms(frame->best_effort_timestamp,
                                   fmt->streams[stream_index]->time_base);
            if (progress_cb != NULL && pts_ms >= 0)
                progress_cb((uint32_t)pts_ms,
                            duration_ms > 0 ? (uint32_t)duration_ms : 1U,
                            user);
            av_frame_unref(frame);
        }
    }

    audio_playback_drain(&pcm);
    ret = 0;

out:
    audio_pcm_close(&pcm);
    free(out_buf);
    if (frame != NULL)
        av_frame_free(&frame);
    if (packet != NULL)
        av_packet_free(&packet);
    if (swr != NULL)
        swr_free(&swr);
    if (codec != NULL)
        avcodec_free_context(&codec);
    if (fmt != NULL)
        avformat_close_input(&fmt);
    return ret;
}
