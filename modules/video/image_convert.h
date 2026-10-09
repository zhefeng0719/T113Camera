#ifndef IMAGE_CONVERT_H
#define IMAGE_CONVERT_H

#include <stddef.h>                 /* size_t。 */
#include <stdint.h>                 /* int64_t。 */

/* CedarC 输入压缩格式；MJPEG 给摄像头和 AVI 录制使用，H264 给 MP4 播放使用。 */
enum cedar_video_codec {
    CEDAR_VIDEO_CODEC_MJPEG = 0,    /* JPEG/MJPEG 帧。 */
    CEDAR_VIDEO_CODEC_H264,         /* H264/AVC Annex-B 码流。 */
    CEDAR_VIDEO_CODEC_H265,         /* H265/HEVC Annex-B 码流。 */
};

/* CedarC 解码器上下文：对外隐藏具体 CedarC 类型，方便主程序只管 open/decode/close。 */
struct cedar_decoder {
    void *memops;                   /* CedarC 内存操作接口，内部实际是 struct ScMemOpsS*。 */
    void *decoder;                  /* CedarC 解码器句柄，内部实际是 VideoDecoder*。 */
    unsigned int width;             /* 解码器初始化宽度。 */
    unsigned int height;            /* 解码器初始化高度。 */
    enum cedar_video_codec codec;   /* 当前压缩格式，用来区分 MJPG 和 H264/H265 延迟吐帧。 */
    unsigned int direct_frames;     /* 使用 VideoPicture dmabuf fd 直通 G2D 的帧数。 */
    unsigned int copy_frames;       /* fd 不可用时，拷贝到普通 YUV buffer 的帧数。 */
};

/* CedarC 解码后的 YUV420 半平面图像，当前主要对应 NV21/NV12。 */
struct yuv_image {
    unsigned int width;             /* 有效图像宽度。 */
    unsigned int height;            /* 有效图像高度。 */
    unsigned int stride;            /* Y 平面每行字节数；直通时来自 CedarC nLineStride。 */
    unsigned int is_nv21;           /* 1 表示 NV21/VU 顺序，0 表示 NV12/UV 顺序。 */
    unsigned char *data;            /* 回退拷贝模式下的 YUV 数据；直通模式下指向 CedarC pData0。 */
    size_t size;                    /* YUV 数据大小。 */
    int dmabuf_fd;                  /* CedarC 输出 buffer fd；小于 0 表示不可直接给 G2D。 */
    int direct;                     /* 1 表示 picture 由 CedarC 持有，释放时需要 ReturnPicture。 */
    void *owner;                    /* direct=1 时指向 VideoDecoder*。 */
    void *picture;                  /* direct=1 时指向 VideoPicture*。 */
};

/* 按指定压缩格式初始化 CedarC 解码器，只在预览/播放开始时调用一次。 */
int image_convert_open_codec(struct cedar_decoder *ctx,
                             unsigned int width, unsigned int height,
                             enum cedar_video_codec codec);
/* 初始化 CedarC MJPG 解码器，只在预览开始时调用一次。 */
int image_convert_open(struct cedar_decoder *ctx,
                       unsigned int width, unsigned int height);
/* 使用持久化 decoder 解码一包压缩视频数据；1=得到图像，0=需要继续喂包，-1=错误。 */
int image_convert_decode_packet(struct cedar_decoder *ctx,
                                const void *data, size_t size,
                                int64_t pts_ms,
                                struct yuv_image *out);
/* 使用持久化 decoder 解码一帧 MJPG。 */
int image_convert_decode_mjpeg(struct cedar_decoder *ctx,
                               const void *jpeg_data, size_t jpeg_size,
                               struct yuv_image *out);
/* 释放一帧 yuv_image，直通帧会归还给 CedarC，拷贝帧会 free。 */
void image_convert_free_yuv(struct yuv_image *image);
/* 关闭 CedarC 解码器。 */
void image_convert_close(struct cedar_decoder *ctx);

#endif
