/*
 * image_convert.c
 *
 * T113Camera 的 CedarC 解码模块。
 *
 * 当前优化：
 *   1. decoder 持久化，预览开始时初始化一次。
 *   2. 优先使用 VideoPicture.nBufFd 直接交给 G2D。
 *   3. 如果 nBufFd 不可用，自动回退到普通内存拷贝路径。
 */

#include "image_convert.h"          /* cedar_decoder / yuv_image。 */

#include <stdio.h>                  /* fprintf / perror / printf。 */
#include <stdlib.h>                 /* malloc / free。 */
#include <string.h>                 /* memset / memcpy。 */
#include <unistd.h>                 /* access / usleep。 */

#include "memoryAdapter.h"          /* CedarC 内存适配器接口。 */
#include "vdecoder.h"               /* CedarC 视频解码器接口。 */

static int g_vdecoder_plugin_added; /* CedarC 插件只需要在进程内注册一次。 */

/* 读取 VIDEO_SRC=copy，用来临时关闭 CedarC dmabuf 直通，排查色彩/缓存问题。 */
static int force_copy_source(void)
{
    const char *mode = getenv("VIDEO_SRC"); /* 读取运行时调试开关。 */

    return mode && strcmp(mode, "copy") == 0; /* 只有明确写 copy 才强制拷贝。 */
}

/* 读取 VIDEO_IN=external，用来实验是否可以绕过 CedarC stream buffer 拷贝。 */
static int use_external_input_buffer(void)
{
    const char *mode = getenv("VIDEO_IN");  /* 读取输入码流调试开关。 */

    return mode && strcmp(mode, "external") == 0; /* 明确写 external 才启用实验路径。 */
}

/* 判断 CedarC 输出格式是否是 NV21。 */
static int is_picture_nv21(const VideoPicture *pic)
{
    return pic->ePixelFormat == PIXEL_FORMAT_NV21; /* 匹配 NV21。 */
}

/* 判断 CedarC 输出格式是否是 NV12。 */
static int is_picture_nv12(const VideoPicture *pic)
{
    return pic->ePixelFormat == PIXEL_FORMAT_NV12; /* 匹配 NV12。 */
}

/* 把 CedarC 输出的 NV12/NV21 拷贝成连续、紧凑的 YUV420 半平面数据。 */
static int copy_nv12_or_nv21(const VideoPicture *pic, struct yuv_image *out,
                             int is_nv21)
{
    unsigned int y;                  /* 当前正在拷贝的行号。 */
    unsigned int width = (unsigned int)pic->nWidth;       /* 解码图像宽度。 */
    unsigned int height = (unsigned int)pic->nHeight;     /* 解码图像高度。 */
    unsigned int stride = pic->nLineStride > 0 ?          /* CedarC 输出 stride。 */
                          (unsigned int)pic->nLineStride : width;
    unsigned char *src_y = (unsigned char *)pic->pData0;  /* CedarC Y 平面起点。 */
    unsigned char *src_uv = src_y + stride * height;      /* CedarC UV/VU 平面起点。 */
    unsigned char *dst_uv;                                /* 输出 UV/VU 平面起点。 */

    memset(out, 0, sizeof(*out));    /* 清空输出结构。 */
    out->width = width;              /* 保存有效宽度。 */
    out->height = height;            /* 保存有效高度。 */
    out->stride = width;             /* 回退路径整理成紧凑排列。 */
    out->is_nv21 = is_nv21 ? 1U : 0U;/* 保存 UV 顺序。 */
    out->dmabuf_fd = -1;             /* 回退路径没有可用 dmabuf fd。 */
    out->direct = 0;                 /* 回退路径不是 CedarC 直通帧。 */
    out->size = (size_t)width * height * 3U / 2U; /* YUV420 紧凑大小。 */
    out->data = malloc(out->size);   /* 分配普通内存。 */
    if (!out->data) {                /* 分配失败。 */
        perror("malloc yuv");        /* 打印错误。 */
        return -1;                   /* 返回失败。 */
    }

    for (y = 0; y < height; y++)     /* 逐行拷贝 Y 平面。 */
        memcpy(out->data + (size_t)y * width,
               src_y + (size_t)y * stride,
               width);

    dst_uv = out->data + (size_t)width * height; /* UV/VU 紧跟 Y 后。 */
    for (y = 0; y < height / 2; y++) /* 逐行拷贝 UV/VU 平面。 */
        memcpy(dst_uv + (size_t)y * width,
               src_uv + (size_t)y * stride,
               width);

    return 0;                        /* 拷贝成功。 */
}

/* 把 CedarC VideoPicture 直接包装成 yuv_image，不做像素数据拷贝。 */
static int wrap_direct_picture(struct cedar_decoder *ctx, VideoPicture *pic,
                               struct yuv_image *out, int is_nv21)
{
    unsigned int stride = pic->nLineStride > 0 ?
                          (unsigned int)pic->nLineStride :
                          (unsigned int)pic->nWidth; /* CedarC 输出 stride。 */

    memset(out, 0, sizeof(*out));    /* 清空输出结构。 */
    out->width = (unsigned int)pic->nWidth;     /* 有效图像宽度。 */
    out->height = (unsigned int)pic->nHeight;   /* 有效图像高度。 */
    out->stride = stride;            /* 直通时保留 CedarC stride。 */
    out->is_nv21 = is_nv21 ? 1U : 0U;/* 保存 UV 顺序。 */
    out->data = (unsigned char *)pic->pData0;   /* 仅用于调试/回退判断。 */
    out->size = (size_t)stride * out->height * 3U / 2U; /* 按 stride 估算大小。 */
    out->dmabuf_fd = pic->nBufFd;    /* 关键：把 CedarC buffer fd 暴露给 G2D。 */
    out->direct = 1;                 /* 标记这是 CedarC 直通帧。 */
    out->owner = ctx->decoder;       /* 释放时需要用这个 decoder ReturnPicture。 */
    out->picture = pic;              /* 保存 CedarC picture 指针。 */
    ctx->direct_frames++;            /* 统计直通帧数。 */
    return 0;                        /* 包装成功。 */
}

static const char *codec_name(enum cedar_video_codec codec)
{
    switch (codec) {
    case CEDAR_VIDEO_CODEC_H265:
        return "H265";
    case CEDAR_VIDEO_CODEC_H264:
        return "H264";
    case CEDAR_VIDEO_CODEC_MJPEG:
    default:
        return "MJPG";
    }
}

/* 按指定压缩格式初始化 CedarC 解码器，只在预览/播放开始时调用一次。 */
int image_convert_open_codec(struct cedar_decoder *ctx,
                             unsigned int width, unsigned int height,
                             enum cedar_video_codec codec)
{
    VConfig config;                  /* CedarC 解码器配置。 */
    VideoStreamInfo info;            /* 输入码流信息。 */
    int ret;                         /* 函数返回值。 */

    memset(ctx, 0, sizeof(*ctx));    /* 清空解码器上下文。 */
    memset(&config, 0, sizeof(config)); /* 清空配置。 */
    memset(&info, 0, sizeof(info));  /* 清空码流信息。 */

    if (access("/dev/cedar_dev", R_OK | W_OK) != 0) { /* 确认 VE 设备节点可用。 */
        fprintf(stderr, "CedarC device /dev/cedar_dev is not available.\n");
        return -1;                   /* 节点不可用。 */
    }

    if (!g_vdecoder_plugin_added) {  /* 插件只注册一次。 */
        AddVDPlugin();               /* 注册 CedarC 解码插件。 */
        g_vdecoder_plugin_added = 1; /* 标记已注册。 */
    }

    ctx->memops = MemAdapterGetOpsS(); /* 获取 CedarC 内存操作表。 */
    if (!ctx->memops) {              /* 获取失败。 */
        fprintf(stderr, "MemAdapterGetOpsS failed\n");
        return -1;                   /* 返回失败。 */
    }
    CdcMemOpen((struct ScMemOpsS *)ctx->memops); /* 打开 CedarC 内存系统。 */

    ctx->decoder = CreateVideoDecoder(); /* 创建解码器。 */
    if (!ctx->decoder) {             /* 创建失败。 */
        fprintf(stderr, "CreateVideoDecoder failed\n");
        CdcMemClose((struct ScMemOpsS *)ctx->memops); /* 关闭内存系统。 */
        memset(ctx, 0, sizeof(*ctx));/* 清空上下文。 */
        return -1;                   /* 返回失败。 */
    }

    if (codec == CEDAR_VIDEO_CODEC_H265) { /* MP4/HEVC 播放链路。 */
        info.eCodecFormat = VIDEO_CODEC_FORMAT_H265; /* 输入是 H265 Annex-B。 */
        info.bIsFramePackage = 1;    /* FFmpeg 每包输出完整访问单元。 */
    } else if (codec == CEDAR_VIDEO_CODEC_H264) { /* MP4/AVC 播放链路。 */
        info.eCodecFormat = VIDEO_CODEC_FORMAT_H264; /* 输入是 H264 Annex-B。 */
        info.bIsFramePackage = 1;    /* FFmpeg 每包输出完整访问单元。 */
    } else {                         /* 摄像头和录制文件仍走 MJPEG。 */
        info.eCodecFormat = VIDEO_CODEC_FORMAT_MJPEG; /* 输入是 MJPEG/JPEG。 */
        info.bIsFramePackage = 1;    /* 每次提交完整 JPEG 帧。 */
    }
    info.nWidth = (int)width;        /* 输入宽度。 */
    info.nHeight = (int)height;      /* 输入高度。 */
    info.nFrameRate = 30;            /* 目标帧率。 */
    info.nFrameDuration = 33333;     /* 30fps 单帧时长。 */

    config.eOutputPixelFormat = PIXEL_FORMAT_NV21; /* CedarC 仍按当前稳定配置请求输出。 */
    config.nDisplayHoldingFrameBufferNum = 1;      /* 保留显示帧数量。 */
    config.nDecodeSmoothFrameBufferNum = 1;        /* 解码平滑缓冲数量。 */
    config.nFrameBufferNum = 4;                    /* 多留一帧给直通显示。 */
    config.memops = (struct ScMemOpsS *)ctx->memops; /* CedarC 内存接口。 */

    ret = InitializeVideoDecoder((VideoDecoder *)ctx->decoder, &info, &config); /* 初始化。 */
    if (ret != 0) {                  /* 初始化失败。 */
        fprintf(stderr, "InitializeVideoDecoder failed: %d\n", ret);
        image_convert_close(ctx);    /* 统一释放。 */
        return -1;                   /* 返回失败。 */
    }

    ctx->width = width;              /* 记录宽度。 */
    ctx->height = height;            /* 记录高度。 */
    ctx->codec = codec;              /* 记录格式，用于处理 H264/H265 延迟输出。 */
    printf("cedarc      : persistent decoder opened %ux%u %s -> NV21/NV12\n",
           width, height, codec_name(codec)); /* 打印状态。 */
    return 0;                        /* 初始化成功。 */
}

/* 初始化 CedarC MJPG 解码器，只在预览开始时调用一次。 */
int image_convert_open(struct cedar_decoder *ctx,
                       unsigned int width, unsigned int height)
{
    return image_convert_open_codec(ctx, width, height, CEDAR_VIDEO_CODEC_MJPEG);
}

/* 使用持久化 decoder 解码一包压缩视频数据。 */
int image_convert_decode_packet(struct cedar_decoder *ctx,
                                const void *data, size_t size,
                                int64_t pts_ms,
                                struct yuv_image *out)
{
    VideoStreamDataInfo data_info;   /* 输入码流描述。 */
    VideoPicture *picture = NULL;    /* CedarC 输出图像。 */
    char *buf0 = NULL;               /* CedarC stream buffer 第一段。 */
    char *buf1 = NULL;               /* CedarC stream buffer 第二段。 */
    int buf0_size = 0;               /* 第一段大小。 */
    int buf1_size = 0;               /* 第二段大小。 */
    int ret;                         /* 函数返回值。 */
    int i;                           /* 轮询计数。 */
    int is_nv21;                     /* 当前输出是否 NV21。 */
    int external_input;              /* 是否直接提交外部 MJPG 指针。 */

    memset(out, 0, sizeof(*out));    /* 清空输出。 */
    out->dmabuf_fd = -1;             /* 默认没有 fd。 */

    if (!ctx || !ctx->decoder || !ctx->memops || !data || size == 0) /* 确认 decoder 已打开。 */
        return -1;                   /* 未打开就失败。 */

    external_input = use_external_input_buffer(); /* 是否走实验路径。 */
    if (external_input) {            /* 实验：不拷贝到 CedarC stream buffer。 */
        buf0 = (char *)data;         /* 直接提交调用者传入的压缩数据指针。 */
    } else {                         /* 正常路径：请求 CedarC 内部 stream buffer。 */
        ret = RequestVideoStreamBuffer((VideoDecoder *)ctx->decoder, (int)size,
                                       &buf0, &buf0_size, &buf1, &buf1_size, 0);
        if (ret != 0 || (size_t)(buf0_size + buf1_size) < size) {
            fprintf(stderr, "RequestVideoStreamBuffer failed: ret=%d size=%d+%d need=%lu\n",
                    ret, buf0_size, buf1_size, (unsigned long)size);
            return -1;               /* stream buffer 不足。 */
        }

        if ((size_t)buf0_size >= size) { /* 第一段足够。 */
            memcpy(buf0, data, size); /* 拷贝整包。 */
        } else {                     /* 两段环形缓冲。 */
            memcpy(buf0, data, buf0_size); /* 拷贝第一段。 */
            memcpy(buf1, (const unsigned char *)data + buf0_size,
                   size - (size_t)buf0_size); /* 拷贝第二段。 */
        }
    }

    memset(&data_info, 0, sizeof(data_info)); /* 清空码流描述。 */
    data_info.pData = buf0;          /* 码流起点。 */
    data_info.nLength = (int)size;   /* 码流长度。 */
    data_info.nPts = pts_ms >= 0 ? pts_ms * 1000 : 0; /* CedarC demo 使用微秒级 PTS。 */
    data_info.bIsFirstPart = 1;      /* 完整帧第一段。 */
    data_info.bIsLastPart = 1;       /* 完整帧最后一段。 */
    data_info.bValid = 1;            /* 数据有效。 */

    ret = SubmitVideoStreamData((VideoDecoder *)ctx->decoder, &data_info, 0);
    if (ret != 0) {                  /* 提交失败。 */
        fprintf(stderr, "SubmitVideoStreamData failed: %d\n", ret);
        return -1;                   /* 返回失败。 */
    }

    for (i = 0; i < 40; i++) {       /* 最多等待约 400ms。 */
        ret = DecodeVideoStream((VideoDecoder *)ctx->decoder, 0, 0, 0, 0);
        if (ret < 0) {               /* 解码出错。 */
            fprintf(stderr, "DecodeVideoStream failed: %d\n", ret);
            break;                   /* 跳出轮询。 */
        }
        picture = RequestPicture((VideoDecoder *)ctx->decoder, 0); /* 尝试取图。 */
        if (picture)                 /* 取到图像。 */
            break;                   /* 结束轮询。 */
        usleep(10000);               /* 等 10ms 再试。 */
    }

    if (!picture) {                  /* 没有取到图像。 */
        if (ctx->codec == CEDAR_VIDEO_CODEC_MJPEG) { /* MJPG 一包就是一帧。 */
            fprintf(stderr, "no decoded picture from CedarC\n");
            return -1;               /* MJPG 没图按错误处理。 */
        }
        return 0;                    /* H264/H265 有 B 帧/重排序延迟，继续喂包。 */
    }

    CdcMemFlushCache((struct ScMemOpsS *)ctx->memops,
                     picture->pData0, picture->nBufSize); /* 同步解码输出缓存。 */

    if (!is_picture_nv21(picture) && !is_picture_nv12(picture)) { /* 只支持 NV21/NV12。 */
        fprintf(stderr, "unsupported CedarC pixel format: %d\n",
                picture->ePixelFormat);
        ReturnPicture((VideoDecoder *)ctx->decoder, picture); /* 归还图像。 */
        return -1;                   /* 返回失败。 */
    }

    is_nv21 = is_picture_nv21(picture); /* 保存 UV 顺序。 */

    if (!force_copy_source() && picture->nBufFd >= 0) { /* 默认优先 CedarC dmabuf 直通。 */
        ret = wrap_direct_picture(ctx, picture, out, is_nv21); /* 直接交给 G2D。 */
        return ret == 0 ? 1 : -1;    /* 新语义：1 表示已经拿到一帧图。 */
    }

    ret = copy_nv12_or_nv21(picture, out, is_nv21); /* fd 不可用，走拷贝回退。 */
    ReturnPicture((VideoDecoder *)ctx->decoder, picture); /* 拷贝完成后归还。 */
    if (ret == 0)                    /* 回退拷贝成功。 */
        ctx->copy_frames++;          /* 统计拷贝帧数。 */
    return ret == 0 ? 1 : -1;        /* 新语义：1 表示已经拿到一帧图。 */
}

/* 使用持久化 decoder 解码一帧 MJPG。 */
int image_convert_decode_mjpeg(struct cedar_decoder *ctx,
                               const void *jpeg_data, size_t jpeg_size,
                               struct yuv_image *out)
{
    int ret = image_convert_decode_packet(ctx, jpeg_data, jpeg_size, -1, out);
    return ret > 0 ? 0 : -1;
}

/* 释放一帧 yuv_image。 */
void image_convert_free_yuv(struct yuv_image *image)
{
    if (!image)                      /* 允许 NULL。 */
        return;                      /* NULL 直接返回。 */

    if (image->direct && image->owner && image->picture) { /* 直通帧属于 CedarC。 */
        ReturnPicture((VideoDecoder *)image->owner,
                      (VideoPicture *)image->picture); /* 归还给 CedarC。 */
    } else {                         /* 回退拷贝帧属于 malloc。 */
        free(image->data);           /* 释放普通内存。 */
    }

    memset(image, 0, sizeof(*image));/* 清空结构，避免二次释放。 */
    image->dmabuf_fd = -1;           /* 标记 fd 无效。 */
}

/* 关闭 CedarC 解码器。 */
void image_convert_close(struct cedar_decoder *ctx)
{
    if (!ctx)                        /* 允许 NULL。 */
        return;                      /* NULL 直接返回。 */

    if (ctx->decoder) {              /* 如果 decoder 已创建。 */
        DestroyVideoDecoder((VideoDecoder *)ctx->decoder); /* 销毁 decoder。 */
        ctx->decoder = NULL;         /* 清空句柄。 */
    }

    if (ctx->memops) {               /* 如果内存系统已打开。 */
        CdcMemClose((struct ScMemOpsS *)ctx->memops); /* 关闭内存系统。 */
        ctx->memops = NULL;          /* 清空指针。 */
    }

    printf("cedarc      : direct=%u copy=%u\n",
           ctx->direct_frames, ctx->copy_frames); /* 打印直通/拷贝统计。 */
    memset(ctx, 0, sizeof(*ctx));    /* 清空上下文。 */
}
