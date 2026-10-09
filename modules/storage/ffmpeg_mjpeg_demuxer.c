/*
 * ffmpeg_mjpeg_demuxer.c
 *
 * 从 MKV/AVI/MP4 等容器中取出压缩视频帧。
 * 注意：这里不解码，只把 packet 交给 CedarC，避免走 FFmpeg 软件解码。
 */

#include "ffmpeg_mjpeg_demuxer.h"

#include <stdio.h>
#include <string.h>

static const char *codec_name(enum ffmpeg_video_codec codec)
{
    switch (codec) {
    case FFMPEG_VIDEO_CODEC_MJPEG:
        return "MJPEG";
    case FFMPEG_VIDEO_CODEC_H264:
        return "H264";
    case FFMPEG_VIDEO_CODEC_H265:
        return "H265";
    default:
        return "UNKNOWN";
    }
}

static int64_t rescale_to_ms(int64_t value, AVRational time_base)
{
    if (value == AV_NOPTS_VALUE)
        return -1;
    return av_rescale_q(value, time_base, (AVRational){1, 1000});
}

int ffmpeg_mjpeg_demuxer_open(struct ffmpeg_mjpeg_demuxer *demuxer,
                              const char *path)
{
    unsigned int i;

    if (demuxer == NULL || path == NULL)
        return -1;

    memset(demuxer, 0, sizeof(*demuxer));
    demuxer->video_stream = -1;
    demuxer->duration_ms = -1;
    av_init_packet(&demuxer->packet);

    if (avformat_open_input(&demuxer->fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "ffmpeg: open input failed: %s\n", path);
        return -1;
    }

    if (avformat_find_stream_info(demuxer->fmt, NULL) < 0) {
        fprintf(stderr, "ffmpeg: find stream info failed\n");
        ffmpeg_mjpeg_demuxer_close(demuxer);
        return -1;
    }

    for (i = 0; i < demuxer->fmt->nb_streams; i++) {
        AVStream *stream = demuxer->fmt->streams[i];
        AVCodecParameters *par = stream->codecpar;

        if (par->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        if (par->codec_id == AV_CODEC_ID_MJPEG)
            demuxer->codec = FFMPEG_VIDEO_CODEC_MJPEG;
        else if (par->codec_id == AV_CODEC_ID_H264)
            demuxer->codec = FFMPEG_VIDEO_CODEC_H264;
        else if (par->codec_id == AV_CODEC_ID_HEVC)
            demuxer->codec = FFMPEG_VIDEO_CODEC_H265;
        else
            continue;

        demuxer->video_stream = (int)i;
        demuxer->time_base = stream->time_base;
        demuxer->width = (unsigned int)par->width;
        demuxer->height = (unsigned int)par->height;
        if (stream->duration != AV_NOPTS_VALUE)
            demuxer->duration_ms = rescale_to_ms(stream->duration,
                                                 stream->time_base);
        break;
    }

    if (demuxer->video_stream < 0) {
        fprintf(stderr, "ffmpeg: no supported video stream found\n");
        ffmpeg_mjpeg_demuxer_close(demuxer);
        return -1;
    }

    if (demuxer->codec == FFMPEG_VIDEO_CODEC_H264 ||
        demuxer->codec == FFMPEG_VIDEO_CODEC_H265) {
        const AVBitStreamFilter *filter;
        AVStream *stream = demuxer->fmt->streams[demuxer->video_stream];

        filter = av_bsf_get_by_name(demuxer->codec == FFMPEG_VIDEO_CODEC_H265 ?
                                    "hevc_mp4toannexb" : "h264_mp4toannexb");
        if (filter == NULL) {
            fprintf(stderr, "ffmpeg: annexb bitstream filter is missing\n");
            ffmpeg_mjpeg_demuxer_close(demuxer);
            return -1;
        }
        if (av_bsf_alloc(filter, &demuxer->bsf) < 0 ||
            avcodec_parameters_copy(demuxer->bsf->par_in, stream->codecpar) < 0 ||
            av_bsf_init(demuxer->bsf) < 0) {
            fprintf(stderr, "ffmpeg: annexb bitstream filter init failed\n");
            ffmpeg_mjpeg_demuxer_close(demuxer);
            return -1;
        }
        av_init_packet(&demuxer->filtered_packet);
    }

    if (demuxer->duration_ms < 0 && demuxer->fmt->duration != AV_NOPTS_VALUE)
        demuxer->duration_ms = demuxer->fmt->duration / 1000;

    printf("ffmpeg demux: stream=%d codec=%s %ux%u duration=%lldms\n",
           demuxer->video_stream,
           codec_name(demuxer->codec),
           demuxer->width,
           demuxer->height,
           (long long)demuxer->duration_ms);
    return 0;
}

int ffmpeg_mjpeg_demuxer_read(struct ffmpeg_mjpeg_demuxer *demuxer,
                              const unsigned char **data,
                              size_t *size,
                              int64_t *pts_ms,
                              int64_t *duration_ms)
{
    int ret;

    if (data != NULL)
        *data = NULL;
    if (size != NULL)
        *size = 0;
    if (pts_ms != NULL)
        *pts_ms = -1;
    if (duration_ms != NULL)
        *duration_ms = -1;
    if (demuxer == NULL || demuxer->fmt == NULL)
        return -1;

    if (demuxer->filtered_packet_valid) {
        av_packet_unref(&demuxer->filtered_packet);
        demuxer->filtered_packet_valid = 0;
    }
    if (demuxer->packet_valid) {
        av_packet_unref(&demuxer->packet);
        demuxer->packet_valid = 0;
    }

    while ((ret = av_read_frame(demuxer->fmt, &demuxer->packet)) >= 0) {
        AVPacket *pkt = &demuxer->packet;

        if (pkt->stream_index != demuxer->video_stream) {
            av_packet_unref(pkt);
            continue;
        }

        if (demuxer->bsf != NULL) {
            ret = av_bsf_send_packet(demuxer->bsf, pkt);
            av_packet_unref(pkt);
            if (ret < 0)
                return -1;
            ret = av_bsf_receive_packet(demuxer->bsf, &demuxer->filtered_packet);
            if (ret == AVERROR(EAGAIN))
                continue;
            if (ret < 0)
                return -1;

            demuxer->filtered_packet_valid = 1;
            pkt = &demuxer->filtered_packet;
        } else {
            demuxer->packet_valid = 1;
        }

        if (data != NULL)
            *data = pkt->data;
        if (size != NULL)
            *size = (size_t)pkt->size;
        if (pts_ms != NULL)
            *pts_ms = rescale_to_ms(pkt->pts, demuxer->time_base);
        if (duration_ms != NULL)
            *duration_ms = rescale_to_ms(pkt->duration, demuxer->time_base);
        return 1;
    }

    return (ret == AVERROR_EOF) ? 0 : -1;
}

void ffmpeg_mjpeg_demuxer_close(struct ffmpeg_mjpeg_demuxer *demuxer)
{
    if (demuxer == NULL)
        return;

    if (demuxer->filtered_packet_valid) {
        av_packet_unref(&demuxer->filtered_packet);
        demuxer->filtered_packet_valid = 0;
    }
    if (demuxer->packet_valid) {
        av_packet_unref(&demuxer->packet);
        demuxer->packet_valid = 0;
    }
    if (demuxer->bsf != NULL)
        av_bsf_free(&demuxer->bsf);
    if (demuxer->fmt != NULL)
        avformat_close_input(&demuxer->fmt);
    memset(demuxer, 0, sizeof(*demuxer));
    demuxer->video_stream = -1;
}
