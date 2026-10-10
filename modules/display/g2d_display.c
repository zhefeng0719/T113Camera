/*
 * g2d_display.c
 *
 * T113Camera 的 G2D 显示模块。
 *
 * 最终预览链路：
 *   CedarC 输出 NV21/NV12
 *   -> 优先把 CedarC dmabuf fd 直接交给 G2D
 *   -> fd 不可用时才拷贝到 ION 源缓冲
 *   -> G2D 完成 YUV->ARGB8888 和 640x480->800x480 缩放
 *   -> 主链路把 ARGB8888 dmabuf 直接交给 /dev/disp layer
 */

#include "g2d_display.h"            /* 本模块结构体和函数声明。 */

#include <fcntl.h>                  /* open。 */
#include <stdio.h>                  /* fprintf / perror / printf。 */
#include <stdlib.h>                 /* getenv。 */
#include <string.h>                 /* memset / memcpy。 */
#include <sys/ioctl.h>              /* ioctl。 */
#include <unistd.h>                 /* close。 */

#include <ion_mem_alloc.h>          /* 全志 ION/libawion 接口。 */

/* 全志增强版 G2D 像素格式枚举，本测试只保留需要用到的格式。 */
typedef enum {
    G2D_FORMAT_ARGB8888 = 0,        /* 目标缓冲使用 ARGB8888。 */
    G2D_FORMAT_ABGR8888 = 1,        /* 目标缓冲使用 ABGR8888。 */
    G2D_FORMAT_RGBA8888 = 2,        /* 目标缓冲使用 RGBA8888。 */
    G2D_FORMAT_BGRA8888 = 3,        /* 目标缓冲使用 BGRA8888。 */
    G2D_FORMAT_XRGB8888 = 4,        /* 目标缓冲使用 XRGB8888。 */
    G2D_FORMAT_RGB888 = 8,          /* 目标缓冲使用 RGB888。 */
    G2D_FORMAT_YUV420UVC_V1U1V0U0 = 0x28, /* NV21/VU 顺序。 */
    G2D_FORMAT_YUV420UVC_U1V1U0V0 = 0x29, /* NV12/UV 顺序。 */
} g2d_fmt_enh;

/* G2D blit 标志，当前不做旋转、镜像、ROP 等额外操作。 */
typedef enum {
    G2D_BLT_NONE_H = 0x0,           /* 普通 blit。 */
} g2d_blt_flags_h;

/* G2D alpha 模式，当前按像素 alpha 模式填默认值。 */
typedef enum {
    G2D_PIXEL_ALPHA = 0x0,          /* 使用像素自身 alpha。 */
} g2d_alpha_mode_enh;

/* G2D 矩形区域。 */
typedef struct {
    int x;                          /* 左上角 x。 */
    int y;                          /* 左上角 y。 */
    unsigned int w;                 /* 矩形宽度。 */
    unsigned int h;                 /* 矩形高度。 */
} g2d_rect;

/* G2D 尺寸结构。 */
typedef struct {
    unsigned int w;                 /* 宽度。 */
    unsigned int h;                 /* 高度。 */
} g2d_size;

/* G2D 坐标结构。 */
typedef struct {
    unsigned int x;                 /* x 坐标。 */
    unsigned int y;                 /* y 坐标。 */
} g2d_coor;

/* G2D 颜色范围。 */
enum color_range {
    COLOR_RANGE_0_255 = 0,          /* 全范围 YUV/RGB。 */
    COLOR_RANGE_16_235 = 1,         /* 视频范围 YUV。 */
};

/* G2D 增强版图像描述，字段顺序必须和 SDK 头文件一致。 */
typedef struct {
    int bbuff;                      /* buffer 标志，按 SDK sample 填 1。 */
    unsigned int color;             /* 颜色键/填充色，当前不用。 */
    g2d_fmt_enh format;             /* 图像像素格式。 */
    unsigned int laddr[3];          /* 物理地址模式使用，当前不用。 */
    unsigned int haddr[3];          /* 高地址字段，当前不用。 */
    unsigned int width;             /* 图像缓冲宽度。 */
    unsigned int height;            /* 图像缓冲高度。 */
    unsigned int align[3];          /* 对齐字段，当前填 0。 */
    g2d_rect clip_rect;             /* 参与处理的裁剪区域。 */
    g2d_size resize;                /* 可选 resize 字段，当前不用。 */
    g2d_coor coor;                  /* 目标坐标字段，当前不用。 */
    int gamut;                      /* 色域字段，当前默认 0。 */
    int bpremul;                    /* 预乘 alpha 字段，当前默认 0。 */
    unsigned char alpha;            /* 全局 alpha 值。 */
    g2d_alpha_mode_enh mode;        /* alpha 模式。 */
    int fd;                         /* dmabuf fd，当前实际使用这个传 ION 缓冲。 */
    unsigned int use_phy_addr;      /* 是否使用物理地址，当前必须为 0。 */
    enum color_range color_range;   /* YUV/RGB 颜色范围。 */
} g2d_image_enh;

/* G2D bitblt 参数。 */
typedef struct {
    g2d_blt_flags_h flag_h;         /* blit 标志。 */
    g2d_image_enh src_image_h;      /* 源图描述。 */
    g2d_image_enh dst_image_h;      /* 目标图描述。 */
} g2d_blt_h;

#define G2D_CMD_BITBLT_H 0x55       /* 增强版 bitblt ioctl 命令。 */

/* 根据 VIDEO_UV 临时覆盖源 YUV 顺序；默认等价于 VIDEO_UV=nv12。 */
static int select_source_is_nv21(const struct yuv_image *image)
{
    const char *mode = getenv("VIDEO_UV"); /* 读取源格式调试开关。 */

    (void)image;                    /* 当前默认不使用 CedarC 上报值，保持和 VIDEO_UV=nv12 一致。 */
    if (mode && strcmp(mode, "nv12") == 0) /* 强制按 NV12/UV 解释。 */
        return 0;
    if (mode && strcmp(mode, "nv21") == 0) /* 强制按 NV21/VU 解释。 */
        return 1;
    return 0;                              /* 默认按 NV12/UV 解释，等价于 VIDEO_UV=nv12。 */
}

/* 根据 VIDEO_RGB 选择 G2D 输出 RGB 字节序，用来排查红蓝通道互换。 */
static g2d_fmt_enh select_destination_format(const char **name)
{
    const char *mode = getenv("VIDEO_RGB"); /* 读取目标格式调试开关。 */

    if (mode && strcmp(mode, "abgr") == 0) { /* ABGR8888。 */
        *name = "ABGR8888";
        return G2D_FORMAT_ABGR8888;
    }
    if (mode && strcmp(mode, "rgba") == 0) { /* RGBA8888。 */
        *name = "RGBA8888";
        return G2D_FORMAT_RGBA8888;
    }
    if (mode && strcmp(mode, "bgra") == 0) { /* BGRA8888。 */
        *name = "BGRA8888";
        return G2D_FORMAT_BGRA8888;
    }
    if (mode && strcmp(mode, "xrgb") == 0) { /* XRGB8888。 */
        *name = "XRGB8888";
        return G2D_FORMAT_XRGB8888;
    }

    *name = "ARGB8888";             /* 默认沿用当前已出画面的格式。 */
    return G2D_FORMAT_ARGB8888;
}

/* 把 G2D 输出的 ARGB8888 转成 framebuffer 的 B/G/R/A 32bpp 布局。 */
static void copy_xrgb8888_to_framebuffer(const struct g2d_display *g2d,
                                         const struct framebuffer_display *fb)
{
    unsigned int x;                 /* 当前像素 x 坐标。 */
    unsigned int y;                 /* 当前像素 y 坐标。 */

    for (y = 0; y < g2d->dst_height && y < fb->height; y++) { /* 遍历每一行。 */
        const unsigned char *src_row =
            (const unsigned char *)g2d->dst_virt + (size_t)y * g2d->dst_stride; /* RGB888 行首。 */
        unsigned char *dst_row = fb->mem + (size_t)y * fb->line_length; /* framebuffer 行首。 */

        for (x = 0; x < g2d->dst_width && x < fb->width; x++) { /* 遍历每个像素。 */
            const unsigned char *src = src_row + x * 4; /* ARGB8888 当前像素。 */
            unsigned char *dst = dst_row + x * 4;      /* framebuffer 当前像素。 */

            dst[0] = src[2];          /* framebuffer 第 0 字节是 B。 */
            dst[1] = src[1];          /* framebuffer 第 1 字节是 G。 */
            dst[2] = src[0];          /* framebuffer 第 2 字节是 R。 */
            dst[3] = 0xff;            /* framebuffer 第 3 字节是 A/保留，填满。 */
        }
    }
}

/* 打开 /dev/g2d，并分配 YUV 源缓冲和 ARGB8888 目标缓冲。 */
int g2d_display_open(struct g2d_display *g2d,
                     unsigned int src_width, unsigned int src_height)
{
    return g2d_display_open_sized(g2d, src_width, src_height, 800, 480);
}

/* 打开 /dev/g2d，并分配指定输出尺寸的 YUV 源缓冲和 ARGB8888 目标缓冲。 */
int g2d_display_open_sized(struct g2d_display *g2d,
                           unsigned int src_width,
                           unsigned int src_height,
                           unsigned int dst_width,
                           unsigned int dst_height)
{
    memset(g2d, 0, sizeof(*g2d));    /* 清空上下文。 */
    g2d->fd = -1;                   /* 初始化为无效 fd。 */
    g2d->src_dmabuf_fd = -1;        /* 初始化为无效 dmabuf fd。 */
    g2d->dst_dmabuf_fd = -1;        /* 初始化为无效 dmabuf fd。 */
    g2d->dst_spare_fd = -1; /* 第二输出页尚未分配；此处不能默认使用 fd 0。 */
    g2d->src_width = src_width;     /* 保存源图宽度。 */
    g2d->src_height = src_height;   /* 保存源图高度。 */
    g2d->src_size = ((src_width + 15U) & ~15U) * ((src_height + 15U) & ~15U) * 3U / 2U; /* 复制回退源按 16 像素对齐，为解码平面填充预留空间。 */
    g2d->dst_width = dst_width;     /* 目标输出宽度。 */
    g2d->dst_height = dst_height;   /* 目标输出高度。 */
    g2d->dst_stride = g2d->dst_width * 4U; /* ARGB8888 每行字节数。 */
    g2d->dst_size = g2d->dst_stride * g2d->dst_height; /* ARGB8888 目标缓冲大小。 */

    g2d->fd = open("/dev/g2d", O_RDWR); /* 打开 G2D 字符设备。 */
    if (g2d->fd < 0) {               /* 打开失败。 */
        perror("/dev/g2d");          /* 打印系统错误。 */
        return -1;                   /* 返回失败。 */
    }

    g2d->memops = GetMemAdapterOpsS(); /* 获取 ION 内存操作接口。 */
    if (!g2d->memops) {              /* 获取失败。 */
        fprintf(stderr, "GetMemAdapterOpsS failed\n");
        g2d_display_close(g2d);      /* 释放已打开资源。 */
        return -1;                   /* 返回失败。 */
    }

    if (SunxiMemOpen(g2d->memops) < 0) { /* 初始化 ION 内存系统。 */
        fprintf(stderr, "SunxiMemOpen failed\n");
        g2d_display_close(g2d);      /* 释放已打开资源。 */
        return -1;                   /* 返回失败。 */
    }

    g2d->src_virt = SunxiMemPalloc(g2d->memops, (int)g2d->src_size); /* 分配源缓冲。 */
    if (!g2d->src_virt) {            /* 分配失败。 */
        fprintf(stderr, "SunxiMemPalloc src failed, size=%u\n", g2d->src_size);
        g2d_display_close(g2d);      /* 释放已打开资源。 */
        return -1;                   /* 返回失败。 */
    }

    g2d->src_dmabuf_fd = SunxiMemGetBufferFd(g2d->memops, g2d->src_virt); /* 获取源 dmabuf fd。 */
    if (g2d->src_dmabuf_fd < 0) {    /* 获取失败。 */
        fprintf(stderr, "SunxiMemGetBufferFd src failed\n");
        g2d_display_close(g2d);      /* 释放已打开资源。 */
        return -1;                   /* 返回失败。 */
    }

    g2d->dst_virt = SunxiMemPalloc(g2d->memops, (int)g2d->dst_size); /* 分配目标缓冲。 */
    if (!g2d->dst_virt) {            /* 分配失败。 */
        fprintf(stderr, "SunxiMemPalloc dst failed, size=%u\n", g2d->dst_size);
        g2d_display_close(g2d);      /* 释放已打开资源。 */
        return -1;                   /* 返回失败。 */
    }

    g2d->dst_dmabuf_fd = SunxiMemGetBufferFd(g2d->memops, g2d->dst_virt); /* 获取目标 dmabuf fd。 */
    if (g2d->dst_dmabuf_fd < 0) {    /* 获取失败。 */
        fprintf(stderr, "SunxiMemGetBufferFd dst failed\n");
        g2d_display_close(g2d);      /* 释放已打开资源。 */
        return -1;                   /* 返回失败。 */
    }
    g2d->dst_spare = SunxiMemPalloc(g2d->memops, (int)g2d->dst_size); /* 分配第二张输出页，G2D 不覆盖 disp 正在扫描的页面。 */
    if (!g2d->dst_spare ||
        (g2d->dst_spare_fd = SunxiMemGetBufferFd(g2d->memops, g2d->dst_spare)) < 0) { /* 取得备页的 dmabuf fd，失败时回收所有已分配资源。 */
        g2d_display_close(g2d);
        return -1;
    }

    printf("g2d         : src=%ux%u YUV420 fd=%d bytes=%u\n",
           g2d->src_width, g2d->src_height,
           g2d->src_dmabuf_fd, g2d->src_size); /* 打印源缓冲信息。 */
    printf("g2d         : dst=%ux%u ARGB8888 fd=%d bytes=%u\n",
           g2d->dst_width, g2d->dst_height,
           g2d->dst_dmabuf_fd, g2d->dst_size); /* 打印目标缓冲信息。 */
    return 0;                        /* 打开成功。 */
}

/* 释放 G2D 和 ION 资源。 */
void g2d_display_close(struct g2d_display *g2d)
{
    if (!g2d)                        /* 允许传入 NULL。 */
        return;                      /* NULL 直接返回。 */

    if (g2d->src_virt && g2d->memops) { /* 如果源缓冲已分配。 */
        SunxiMemPfree(g2d->memops, g2d->src_virt); /* 释放源缓冲。 */
        g2d->src_virt = NULL;        /* 清空指针。 */
    }

    if (g2d->dst_virt && g2d->memops) { /* 如果目标缓冲已分配。 */
        SunxiMemPfree(g2d->memops, g2d->dst_virt); /* 释放目标缓冲。 */
        g2d->dst_virt = NULL;        /* 清空指针。 */
    }
    if (g2d->dst_spare && g2d->memops) {
        SunxiMemPfree(g2d->memops, g2d->dst_spare); /* 显示图层已停用后释放第二输出页。 */
        g2d->dst_spare = NULL;
    }

    if (g2d->memops) {               /* 如果 ION 内存系统已打开。 */
        SunxiMemClose(g2d->memops);  /* 关闭 ION 内存系统。 */
        g2d->memops = NULL;          /* 清空接口指针。 */
    }

    if (g2d->fd >= 0) {              /* 如果 /dev/g2d 已打开。 */
        close(g2d->fd);              /* 关闭设备 fd。 */
        g2d->fd = -1;                /* 标记为无效。 */
    }
}

/* 把 NV21/NV12 图像交给 G2D，完成 YUV->ARGB8888 和 800x480 缩放。 */
int g2d_display_stretch_yuv_to_dmabuf(struct g2d_display *g2d,
                                      const struct yuv_image *image)
{
    g2d_blt_h blt;                   /* G2D blit 参数。 */
    int src_fd;                      /* 本帧交给 G2D 的源 dmabuf fd。 */
    int src_is_nv21;                 /* 本帧交给 G2D 时使用的 UV 顺序。 */
    const char *dst_format_name;     /* G2D 输出格式名称，仅用于日志。 */
    g2d_fmt_enh dst_format;          /* G2D 输出 RGB 格式。 */

    if (!g2d || !image || !g2d->dst_virt || !image->data)
        return -1;                   /* 任一关键指针无效就失败。 */
    {
        void *virt = g2d->dst_virt; /* 暂存上一输出页的 CPU 映射地址。 */
        int fd = g2d->dst_dmabuf_fd; /* 同时保存对应 fd，虚拟地址和 fd 必须成对交换。 */
        g2d->dst_virt = g2d->dst_spare; /* 本次 G2D 写入另一页，不改写当前扫描页。 */
        g2d->dst_dmabuf_fd = g2d->dst_spare_fd; /* 选择本次输出页的 dmabuf。 */
        g2d->dst_spare = virt; /* 旧输出页成为下一次备页，依赖上层等到真实 VSYNC 后才复用。 */
        g2d->dst_spare_fd = fd; /* 保持备页地址与 fd 一致。 */
    }

    if (image->dmabuf_fd >= 0) {     /* CedarC 已经提供可直接给 G2D 的 fd。 */
        src_fd = image->dmabuf_fd;   /* 直接使用 CedarC 输出 buffer。 */
    } else {                         /* fd 不可用，回退到 ION 源缓冲。 */
        if (!g2d->src_virt)          /* 没有回退缓冲。 */
            return -1;               /* 返回失败。 */
        if (image->size > g2d->src_size) { /* 确认源缓冲足够放当前 YUV。 */
            fprintf(stderr, "g2d yuv source buffer too small: need=%lu have=%u\n",
                    (unsigned long)image->size, g2d->src_size);
            return -1;               /* 缓冲不足直接失败。 */
        }
        memcpy(g2d->src_virt, image->data, image->size); /* 把 YUV 拷贝到 ION 源缓冲。 */
        SunxiMemFlushCache(g2d->memops, g2d->src_virt, (int)image->size); /* 刷新源缓冲 cache。 */
        src_fd = g2d->src_dmabuf_fd; /* 使用回退 ION 源缓冲 fd。 */
    }

    memset(&blt, 0, sizeof(blt));    /* 清空 G2D 参数结构。 */
    blt.flag_h = G2D_BLT_NONE_H;     /* 普通 blit。 */
    src_is_nv21 = select_source_is_nv21(image); /* 根据环境变量确定源格式。 */
    dst_format = select_destination_format(&dst_format_name); /* 确定目标 RGB 格式。 */

    blt.src_image_h.bbuff = 1;       /* 按 SDK sample 填 1。 */
    blt.src_image_h.format = src_is_nv21 ? /* 根据实际 UV 顺序选择 G2D 格式。 */
        G2D_FORMAT_YUV420UVC_V1U1V0U0 : /* NV21 按 VU 顺序解释色度。 */
        G2D_FORMAT_YUV420UVC_U1V1U0V0; /* NV12 按 UV 顺序解释色度；错误选择会使颜色交换。 */
    blt.src_image_h.width = image->stride;     /* 源缓冲宽度，直通时可能大于有效宽度。 */
    blt.src_image_h.height = image->buffer_height ? image->buffer_height : image->height; /* 按分配高度定位色度平面，不能仅填有效显示高度。 */
    blt.src_image_h.clip_rect.x = image->crop_x; /* 源图有效区域横向偏移，排除解码填充。 */
    blt.src_image_h.clip_rect.y = image->crop_y; /* 源图有效区域纵向偏移。 */
    blt.src_image_h.clip_rect.w = image->width;/* 使用完整源图宽度。 */
    blt.src_image_h.clip_rect.h = image->height;/* 使用完整源图高度。 */
    blt.src_image_h.mode = G2D_PIXEL_ALPHA;    /* 默认 alpha 模式。 */
    blt.src_image_h.alpha = 255;               /* 不做透明。 */
    blt.src_image_h.fd = src_fd;               /* 使用 dmabuf fd 传源缓冲。 */
    blt.src_image_h.color_range = COLOR_RANGE_0_255; /* 当前按全范围处理。 */

    blt.dst_image_h.bbuff = 1;       /* 按 SDK sample 填 1。 */
    blt.dst_image_h.format = dst_format;/* 目标输出格式，可通过 VIDEO_RGB 临时切换。 */
    blt.dst_image_h.width = g2d->dst_width;    /* 目标缓冲宽度。 */
    blt.dst_image_h.height = g2d->dst_height;  /* 目标缓冲高度。 */
    blt.dst_image_h.clip_rect.x = 0;           /* 写目标左上角。 */
    blt.dst_image_h.clip_rect.y = 0;           /* 写目标左上角。 */
    blt.dst_image_h.clip_rect.w = g2d->dst_width; /* 输出完整屏幕宽度。 */
    blt.dst_image_h.clip_rect.h = g2d->dst_height;/* 输出完整屏幕高度。 */
    blt.dst_image_h.mode = G2D_PIXEL_ALPHA;    /* 默认 alpha 模式。 */
    blt.dst_image_h.alpha = 255;               /* 不做透明。 */
    blt.dst_image_h.fd = g2d->dst_dmabuf_fd;   /* 使用 dmabuf fd 传目标缓冲。 */
    blt.dst_image_h.color_range = COLOR_RANGE_0_255; /* 目标 RGB 全范围。 */

    if (ioctl(g2d->fd, G2D_CMD_BITBLT_H, (unsigned long)&blt) < 0) { /* 提交 G2D。 */
        perror("G2D_CMD_BITBLT_H yuv"); /* 打印 ioctl 错误。 */
        return -1;                   /* 返回失败。 */
    }

    SunxiMemFlushCache(g2d->memops, g2d->dst_virt, (int)g2d->dst_size); /* 同步 G2D 输出缓冲给 disp。 */

    {
        static int printed_once;      /* 只打印一次，避免实时预览刷屏。 */
        if (!printed_once) {          /* 第一次显示时输出路径信息。 */
            printf("g2d source  : %s fd=%d stride=%u size=%lu\n",
                   image->direct ? "cedarc-dmabuf" : "ion-copy",
                   src_fd, image->stride, (unsigned long)image->size);
            printf("g2d format  : src=%s dst=%s\n",
                   src_is_nv21 ? "NV21" : "NV12", dst_format_name);
            printed_once = 1;         /* 后续不再重复打印。 */
        }
    }

    return 0;                        /* 显示成功。 */
}

/* 获取 G2D 输出 dmabuf fd，给 /dev/disp layer 使用。 */
int g2d_display_get_dst_fd(const struct g2d_display *g2d)
{
    return g2d ? g2d->dst_dmabuf_fd : -1; /* 返回目标 buffer fd。 */
}

/* 获取 G2D 输出宽度。 */
unsigned int g2d_display_get_dst_width(const struct g2d_display *g2d)
{
    return g2d ? g2d->dst_width : 0; /* 返回目标宽度。 */
}

/* 获取 G2D 输出高度。 */
unsigned int g2d_display_get_dst_height(const struct g2d_display *g2d)
{
    return g2d ? g2d->dst_height : 0; /* 返回目标高度。 */
}

/* 旧 framebuffer fallback：G2D 输出后再由 CPU 拷贝到 /dev/fb0。 */
int g2d_display_stretch_yuv_to_fb(struct g2d_display *g2d,
                                  const struct yuv_image *image,
                                  const struct framebuffer_display *fb)
{
    if (!fb)                         /* framebuffer 参数无效。 */
        return -1;                   /* 返回失败。 */
    if (g2d_display_stretch_yuv_to_dmabuf(g2d, image) < 0) /* 先输出到 dmabuf。 */
        return -1;                   /* G2D 失败。 */
    copy_xrgb8888_to_framebuffer(g2d, fb); /* CPU 拷贝到 framebuffer。 */
    return 0;                        /* fallback 显示成功。 */
}
