#ifndef G2D_DISPLAY_H
#define G2D_DISPLAY_H

#include "framebuffer_display.h"    /* framebuffer_display 结构体，保留旧 fb fallback 接口。 */
#include "image_convert.h"          /* yuv_image 结构体。 */

/* G2D 显示上下文：一个复制回退源缓冲和两张交替输出 RGB 页。 */
struct g2d_display {
    int fd;                         /* /dev/g2d 文件描述符。 */
    struct SunxiMemOpsS *memops;    /* libawion 内存操作接口。 */
    void *src_virt;                 /* 源 YUV 图像 ION 虚拟地址。 */
    void *dst_virt;                 /* 目标 ARGB8888 图像 ION 虚拟地址。 */
    int src_dmabuf_fd;              /* 源图 ION dmabuf fd。 */
    int dst_dmabuf_fd;              /* 目标图 ION dmabuf fd。 */
    void *dst_spare; /* 另一张 RGB 输出页的映射地址，与 spare_fd 成对。 */
    int dst_spare_fd;               /* Alternate output; never render into the scanning buffer. */
    unsigned int src_width;         /* 源图宽度。 */
    unsigned int src_height;        /* 源图高度。 */
    unsigned int src_size;          /* 源图 ION 缓冲大小。 */
    unsigned int dst_width;         /* 目标图宽度，当前为屏幕宽 800。 */
    unsigned int dst_height;        /* 目标图高度，当前为屏幕高 480。 */
    unsigned int dst_stride;        /* 目标 ARGB8888 每行字节数。 */
    unsigned int dst_size;          /* 目标 ARGB8888 ION 缓冲大小。 */
};

/* 打开 /dev/g2d 并分配 ION 源/目标缓冲。 */
int g2d_display_open(struct g2d_display *g2d,
                     unsigned int src_width, unsigned int src_height);
/* 打开 /dev/g2d，并指定目标 ARGB8888 输出尺寸。 */
int g2d_display_open_sized(struct g2d_display *g2d,
                           unsigned int src_width,
                           unsigned int src_height,
                           unsigned int dst_width,
                           unsigned int dst_height);
/* 关闭 /dev/g2d 并释放 ION 缓冲。 */
void g2d_display_close(struct g2d_display *g2d);
/* 使用 G2D 把 NV21/NV12 转成 ARGB8888，缩放后输出到内部 dmabuf。 */
int g2d_display_stretch_yuv_to_dmabuf(struct g2d_display *g2d,
                                      const struct yuv_image *image);
/* 获取 G2D 输出 dmabuf fd，给 /dev/disp layer 使用。 */
int g2d_display_get_dst_fd(const struct g2d_display *g2d);
/* 获取 G2D 输出宽度。 */
unsigned int g2d_display_get_dst_width(const struct g2d_display *g2d);
/* 获取 G2D 输出高度。 */
unsigned int g2d_display_get_dst_height(const struct g2d_display *g2d);
/* 使用 G2D 把 NV21/NV12 转成 XRGB8888，缩放后再 CPU 写入 framebuffer。 */
int g2d_display_stretch_yuv_to_fb(struct g2d_display *g2d,
                                  const struct yuv_image *image,
                                  const struct framebuffer_display *fb);

#endif
