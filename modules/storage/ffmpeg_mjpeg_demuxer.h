#ifndef FFMPEG_MJPEG_DEMUXER_H
#define FFMPEG_MJPEG_DEMUXER_H

#include <stddef.h>
#include <stdint.h>

#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>
#include "ffmpeg_io.h"

enum ffmpeg_video_codec {
    FFMPEG_VIDEO_CODEC_UNKNOWN = 0,
    FFMPEG_VIDEO_CODEC_MJPEG,
    FFMPEG_VIDEO_CODEC_H264,
    FFMPEG_VIDEO_CODEC_H265,
};

struct ffmpeg_mjpeg_demuxer {
    AVFormatContext *fmt; /* 解封装上下文，只由所属任务读写。 */
    struct ffmpeg_io_guard io;
    int video_stream; /* 支持的视频流索引，-1 表示没有。 */
    AVRational time_base; /* 所选流 PTS 的时间刻度。 */
    AVPacket packet; /* 本轮原始 packet，read 返回借用其负载。 */
    AVPacket filtered_packet;
    AVBSFContext *bsf;
    int packet_valid; /* 负载引用有效时关闭或下次 read 必须 unref。 */
    int filtered_packet_valid;
    int64_t duration_ms; /* 媒体总时长，未知时为负值。 */
    unsigned int width; /* 容器声明宽度，输出对齐几何另由解码器提供。 */
    unsigned int height; /* 容器声明高度，不等于 YUV 缓冲分配高度。 */
    enum ffmpeg_video_codec codec; /* CedarC 输入 codec 类型。 */
};

int ffmpeg_mjpeg_demuxer_open(struct ffmpeg_mjpeg_demuxer *demuxer,
                              const char *path);
int ffmpeg_mjpeg_demuxer_open_interruptible(struct ffmpeg_mjpeg_demuxer *demuxer,
                                            const char *path, volatile int *stop);
int ffmpeg_mjpeg_demuxer_read(struct ffmpeg_mjpeg_demuxer *demuxer,
                              const unsigned char **data,
                              size_t *size,
                              int64_t *pts_ms,
                              int64_t *duration_ms);
void ffmpeg_mjpeg_demuxer_close(struct ffmpeg_mjpeg_demuxer *demuxer);

#endif
