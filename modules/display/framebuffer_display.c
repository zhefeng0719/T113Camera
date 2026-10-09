/*
 * framebuffer_display.c
 *
 * T113Camera framebuffer 显示模块。
 *
 * 当前用途：
 *   1. 打开 /dev/fb0。
 *   2. 获取 800x480 / 32bpp / stride 等参数。
 *   3. 给 G2D 显示模块提供 mmap 后的 framebuffer 指针。
 *
 * 当前板端 fbset 显示 rgba 8/16,8/8,8/0,8/24，
 * 也就是 32bpp 内存按 B/G/R/A 字节顺序写入。
 */

#include "framebuffer_display.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* 写一个 BGRA8888 像素到 framebuffer 内存。 */
static void put_pixel_bgra(unsigned char *dst,
                           unsigned char r, unsigned char g, unsigned char b)
{
    dst[0] = b;                      /* blue 位于 bit 0~7。 */
    dst[1] = g;                      /* green 位于 bit 8~15。 */
    dst[2] = r;                      /* red 位于 bit 16~23。 */
    dst[3] = 0xff;                   /* alpha 填满。 */
}

/* 打开 framebuffer 并 mmap 整个显存。 */
int framebuffer_open(struct framebuffer_display *fb, const char *device)
{
    struct fb_var_screeninfo vinfo;  /* 可变屏幕信息：分辨率、色深、虚拟高度。 */
    struct fb_fix_screeninfo finfo;  /* 固定屏幕信息：stride、显存长度。 */

    memset(fb, 0, sizeof(*fb));

    fb->fd = open(device, O_RDWR);
    if (fb->fd < 0) {
        perror(device);
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_VSCREENINFO");
        framebuffer_close(fb);
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("FBIOGET_FSCREENINFO");
        framebuffer_close(fb);
        return -1;
    }

    if (vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "unsupported framebuffer bpp: %u, need 32\n",
                vinfo.bits_per_pixel);
        framebuffer_close(fb);
        return -1;
    }

    fb->width = vinfo.xres;
    fb->height = vinfo.yres;
    fb->virtual_height = vinfo.yres_virtual;
    fb->bpp = vinfo.bits_per_pixel;
    fb->line_length = finfo.line_length;
    fb->phys_addr = finfo.smem_start;
    fb->map_size = finfo.smem_len;

    fb->mem = mmap(NULL, fb->map_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        perror("mmap framebuffer");
        fb->mem = NULL;
        framebuffer_close(fb);
        return -1;
    }

    printf("framebuffer : %ux%u virtual=%ux%u bpp=%u stride=%u phys=0x%lx bytes=%lu\n",
           fb->width, fb->height, fb->width, fb->virtual_height,
           fb->bpp, fb->line_length, fb->phys_addr, (unsigned long)fb->map_size);
    return 0;
}

/* 关闭 framebuffer。 */
void framebuffer_close(struct framebuffer_display *fb)
{
    if (!fb)
        return;
    if (fb->mem) {
        munmap(fb->mem, fb->map_size);
        fb->mem = NULL;
    }
    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
}

/* 用纯色清当前可见屏幕。 */
void framebuffer_clear(struct framebuffer_display *fb, unsigned char r,
                       unsigned char g, unsigned char b)
{
    unsigned int x;                  /* 当前 x 坐标。 */
    unsigned int y;                  /* 当前 y 坐标。 */

    if (!fb || !fb->mem)
        return;

    for (y = 0; y < fb->height; y++) {
        unsigned char *row = fb->mem + (size_t)y * fb->line_length;
        for (x = 0; x < fb->width; x++)
            put_pixel_bgra(row + x * 4, r, g, b);
    }
}

