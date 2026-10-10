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
    char temp_path[520];            /* 未完成录像使用 .avi.part，不进入播放列表。 */
    int file_fd;                   /* 自定义 AVIO 的文件描述符，用于精确 fsync。 */
    int dir_fd;                    /* 固定目标目录，避免拔盘后写入板载文件系统。 */
    int header_written;            /* 只有成功写头之后才能写尾。 */
    int failed;                    /* 粘滞错误：失败录像不得发布为正式 AVI。 */
    const volatile int *cancel;    /* 通用取消信号，拥有者活到封装收尾结束。 */
    unsigned int width;             /* 视频宽度。 */
    unsigned int height;            /* 视频高度。 */
    unsigned int audio_rate;        /* 音频采样率。 */
    unsigned int audio_channels;    /* 音频声道数。 */
    unsigned int audio_bits;        /* 音频采样位宽。 */
    unsigned int fallback_fps;      /* 标称帧率与 packet 缺省持续时间，不代替实际采集 PTS。 */
    unsigned int frame_ms;          /* 兜底每帧毫秒数。 */
    unsigned int frames;            /* 已写入帧数。 */
    unsigned int audio_blocks;      /* 已写入音频块数量。 */
    uint64_t audio_frames;          /* 已写入音频帧数量。 */
    size_t bytes;                   /* 已写入 MJPG 数据字节数。 */
    size_t audio_bytes;             /* 已写入 PCM 数据字节数。 */
    uint64_t start_ms;              /* AVI 写头成功后的统一单调起点，单位毫秒。 */
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
int ffmpeg_mjpeg_muxer_write_at(struct ffmpeg_mjpeg_muxer *muxer,
                                const void *data, size_t size, int64_t capture_us);
int ffmpeg_mjpeg_muxer_write_audio_at(struct ffmpeg_mjpeg_muxer *muxer,
                                      const void *data, size_t size,
                                      unsigned int frames, int64_t capture_us);
void ffmpeg_mjpeg_muxer_abort(struct ffmpeg_mjpeg_muxer *muxer);

#endif
