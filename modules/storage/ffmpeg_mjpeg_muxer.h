#ifndef FFMPEG_MJPEG_MUXER_H
#define FFMPEG_MJPEG_MUXER_H

#include <stddef.h>
#include <stdint.h>

#include <libavformat/avformat.h>

/* FFmpeg MJPEG 封装器：不编码，把 UVC MJPG 和可选 PCM 音频写入 AVI 容器。 */
struct ffmpeg_mjpeg_muxer {
    AVFormatContext *fmt;           /* FFmpeg 输出容器上下文。 */
    AVStream *video_stream;         /* 视频流。 */
    AVStream *audio_stream;         /* 音频流，可为空。 */
    char path[512];                 /* 输出文件完整路径。 */
    unsigned int width;             /* 视频宽度。 */
    unsigned int height;            /* 视频高度。 */
    unsigned int audio_rate;        /* 音频采样率。 */
    unsigned int audio_channels;    /* 音频声道数。 */
    unsigned int audio_bits;        /* 音频采样位宽。 */
    unsigned int fallback_fps;      /* 同一毫秒多帧时使用的兜底帧间隔。 */
    unsigned int frame_ms;          /* 兜底每帧毫秒数。 */
    unsigned int frames;            /* 已写入帧数。 */
    unsigned int audio_blocks;      /* 已写入音频块数量。 */
    uint64_t audio_frames;          /* 已写入音频帧数量。 */
    size_t bytes;                   /* 已写入 MJPG 数据字节数。 */
    size_t audio_bytes;             /* 已写入 PCM 数据字节数。 */
    uint64_t start_ms;              /* 第一帧写入时刻。 */
    int64_t last_pts;               /* 上一帧时间戳。 */
};

int ffmpeg_mjpeg_muxer_open_av_named(struct ffmpeg_mjpeg_muxer *muxer,
                                     const char *usb_root,
                                     const char *base_name,
                                     unsigned int width,
                                     unsigned int height,
                                     unsigned int fallback_fps,
                                     unsigned int audio_rate,
                                     unsigned int audio_channels,
                                     unsigned int audio_bits,
                                     char *out_path,
                                     size_t out_len);
int ffmpeg_mjpeg_muxer_open_named(struct ffmpeg_mjpeg_muxer *muxer,
                                  const char *usb_root,
                                  const char *base_name,
                                  unsigned int width,
                                  unsigned int height,
                                  unsigned int fallback_fps,
                                  char *out_path,
                                  size_t out_len);
int ffmpeg_mjpeg_muxer_open(struct ffmpeg_mjpeg_muxer *muxer,
                            const char *usb_root,
                            unsigned int width,
                            unsigned int height,
                            unsigned int fallback_fps,
                            char *out_path,
                            size_t out_len);
int ffmpeg_mjpeg_muxer_write(struct ffmpeg_mjpeg_muxer *muxer,
                             const void *data,
                             size_t size);
int ffmpeg_mjpeg_muxer_write_audio(struct ffmpeg_mjpeg_muxer *muxer,
                                   const void *data,
                                   size_t size,
                                   unsigned int frames);
int ffmpeg_mjpeg_muxer_close(struct ffmpeg_mjpeg_muxer *muxer);

#endif
