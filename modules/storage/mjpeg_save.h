#ifndef MJPEG_SAVE_H
#define MJPEG_SAVE_H

#include <stddef.h>                 /* size_t。 */
#include <stdio.h>                  /* FILE。 */

/* MJPEG 裸流写入器：每次写入一帧摄像头输出的 MJPG 数据。 */
struct mjpeg_writer {
    FILE *fp;                       /* 输出文件句柄。 */
    char path[512];                 /* 输出文件完整路径。 */
    unsigned int frames;            /* 已写入帧数。 */
    size_t bytes;                   /* 已写入总字节数。 */
    unsigned char *batch;           /* 批量写缓冲，减少频繁 fwrite。 */
    size_t batch_size;              /* 批量写缓冲容量。 */
    size_t batch_used;              /* 批量写缓冲已使用字节数。 */
};

/* 打开 U 盘上的 video.mjpeg 输出文件。 */
int mjpeg_writer_open(struct mjpeg_writer *writer, const char *usb_root,
                      char *out_path, size_t out_len);
/* 打开 U 盘上的指定 MJPEG 输出文件，base_name 不包含目录和扩展名。 */
int mjpeg_writer_open_named(struct mjpeg_writer *writer, const char *usb_root,
                            const char *base_name,
                            char *out_path, size_t out_len);
/* 向 video.mjpeg 追加一帧 MJPG。 */
int mjpeg_writer_write(struct mjpeg_writer *writer, const void *data, size_t size);
/* 关闭 video.mjpeg 并同步到 U 盘。 */
int mjpeg_writer_close(struct mjpeg_writer *writer);

#endif
