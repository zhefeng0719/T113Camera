#ifndef FRAMEBUFFER_DISPLAY_H
#define FRAMEBUFFER_DISPLAY_H

#include <stddef.h>

struct framebuffer_display {
    int fd;                         /* /dev/fb0 文件描述符。 */
    unsigned int width;             /* 可见宽度。 */
    unsigned int height;            /* 可见高度。 */
    unsigned int virtual_height;    /* 虚拟高度，可能是 height 的 2 倍。 */
    unsigned int bpp;               /* 每像素 bit 数。 */
    unsigned int line_length;       /* 每行真实字节数。 */
    unsigned long phys_addr;        /* framebuffer 显存物理地址，给 G2D 使用。 */
    size_t map_size;                /* mmap 的显存大小。 */
    unsigned char *mem;             /* mmap 后的显存首地址。 */
};

int framebuffer_open(struct framebuffer_display *fb, const char *device);
void framebuffer_close(struct framebuffer_display *fb);
void framebuffer_clear(struct framebuffer_display *fb, unsigned char r,
                       unsigned char g, unsigned char b);

#endif
