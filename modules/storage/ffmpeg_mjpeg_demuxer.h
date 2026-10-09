#ifndef FFMPEG_MJPEG_DEMUXER_H
#define FFMPEG_MJPEG_DEMUXER_H

#include <stddef.h>
#include <stdint.h>

#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>

enum ffmpeg_video_codec {
    FFMPEG_VIDEO_CODEC_UNKNOWN = 0,
    FFMPEG_VIDEO_CODEC_MJPEG,
    FFMPEG_VIDEO_CODEC_H264,
    FFMPEG_VIDEO_CODEC_H265,
};

struct ffmpeg_mjpeg_demuxer {
    AVFormatContext *fmt;
    int video_stream;
    AVRational time_base;
    AVPacket packet;
    AVPacket filtered_packet;
    AVBSFContext *bsf;
    int packet_valid;
    int filtered_packet_valid;
    int64_t duration_ms;
    unsigned int width;
    unsigned int height;
    enum ffmpeg_video_codec codec;
};

int ffmpeg_mjpeg_demuxer_open(struct ffmpeg_mjpeg_demuxer *demuxer,
                              const char *path);
int ffmpeg_mjpeg_demuxer_read(struct ffmpeg_mjpeg_demuxer *demuxer,
                              const unsigned char **data,
                              size_t *size,
                              int64_t *pts_ms,
                              int64_t *duration_ms);
void ffmpeg_mjpeg_demuxer_close(struct ffmpeg_mjpeg_demuxer *demuxer);

#endif
