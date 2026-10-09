#ifndef AV_PIPELINE_H
#define AV_PIPELINE_H

#include <stddef.h>                 /* size_t。 */
#include <stdint.h>                 /* int64_t。 */

enum t113_stream_type {             /* 后续封装器要接收的流类型。 */
    T113_STREAM_VIDEO_MJPEG = 0,    /* UVC 摄像头输出的 MJPEG 视频 packet。 */
    T113_STREAM_AUDIO_PCM = 1,      /* ALSA 采集到的 PCM 音频 block。 */
};

struct t113_video_packet {          /* 视频 packet 的最小公共描述。 */
    const unsigned char *data;      /* 指向一帧 MJPEG 数据。 */
    size_t size;                    /* MJPEG 数据字节数。 */
    int64_t pts_ms;                 /* 该帧显示时间戳，单位毫秒。 */
    int64_t duration_ms;            /* 该帧持续时间，单位毫秒。 */
    unsigned int width;             /* 视频宽度，例如 640。 */
    unsigned int height;            /* 视频高度，例如 480。 */
};

struct t113_audio_packet {          /* 音频 packet 的最小公共描述。 */
    const unsigned char *data;      /* 指向一段 PCM 数据。 */
    size_t size;                    /* PCM 数据字节数。 */
    int64_t pts_ms;                 /* 这段音频开始时间戳，单位毫秒。 */
    unsigned int sample_rate;       /* 采样率，例如 48000。 */
    unsigned int channels;          /* 声道数，目前板载麦克风优先保留 1。 */
    unsigned int bits_per_sample;   /* 采样位宽，目前 ALSA 测试使用 16。 */
};

struct t113_muxer_ops {             /* 后续统一封装器的操作接口。 */
    int (*open)(void *ctx, const char *path); /* 打开输出文件，例如 record1.mkv。 */
    int (*write_video)(void *ctx, const struct t113_video_packet *packet); /* 写入一帧视频。 */
    int (*write_audio)(void *ctx, const struct t113_audio_packet *packet); /* 写入一段音频。 */
    int (*close)(void *ctx);        /* 写尾部并关闭文件。 */
};

#endif
