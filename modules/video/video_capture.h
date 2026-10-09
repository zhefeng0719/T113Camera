#ifndef VIDEO_CAPTURE_H
#define VIDEO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* 一帧摄像头数据。data 指向 MJPG 压缩数据，size 是有效字节数。 */
struct video_frame {
    void *data;                     /* 帧数据地址。 */
    size_t size;                    /* 帧数据长度。 */
    unsigned int index;             /* V4L2 mmap 缓冲索引，归还缓冲时使用。 */
    unsigned int sequence;          /* 摄像头帧序号，便于观察是否连续采集。 */
};

/* V4L2 摄像头上下文。调用 open 后由模块维护，调用 close 释放。 */
struct video_capture {
    int fd;                         /* /dev/videoX 文件描述符。 */
    unsigned int width;             /* 实际采集宽度。 */
    unsigned int height;            /* 实际采集高度。 */
    unsigned int pixfmt;            /* 实际像素格式，例如 V4L2_PIX_FMT_MJPEG。 */
    struct video_buffer *buffers;   /* mmap 缓冲数组，定义在 .c 内部。 */
    unsigned int buffer_count;      /* mmap 缓冲数量。 */
    pthread_mutex_t ioctl_mutex;    /* 多线程归还/取帧时保护同一个 V4L2 fd。 */
    int mutex_ready;                /* ioctl_mutex 是否已经初始化。 */
};

/* 打开并配置摄像头。 */
int video_capture_open(struct video_capture *cap, const char *device,
                       unsigned int width, unsigned int height,
                       unsigned int pixfmt);
/* 打印摄像头支持的格式和分辨率。 */
int video_capture_print_capability(const char *device);
/* 启动视频流。 */
int video_capture_start(struct video_capture *cap);
/* 取出一帧，使用完必须调用 video_capture_put_frame。 */
int video_capture_get_frame(struct video_capture *cap, struct video_frame *frame);
/* 归还一帧，让驱动继续复用该缓冲。 */
int video_capture_put_frame(struct video_capture *cap, const struct video_frame *frame);
/* 停止视频流。 */
void video_capture_stop(struct video_capture *cap);
/* 释放 mmap 缓冲并关闭设备。 */
void video_capture_close(struct video_capture *cap);

#endif
