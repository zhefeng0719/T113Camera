/*
 * video_capture.c
 *
 * V4L2 摄像头采集模块。
 *
 * 这个文件只负责一件事：从 /dev/video0 这样的 V4L2 设备里取出一帧一帧的
 * MJPG 数据。它不关心数据保存到哪里，也不关心后面是否解码显示。
 */

#include "video_capture.h"          /* 本模块对外暴露的结构体和函数声明。 */

#include <errno.h>                  /* errno / EINTR，用于判断系统调用错误原因。 */
#include <fcntl.h>                  /* open() 的 O_RDWR、O_NONBLOCK 等标志。 */
#include <linux/videodev2.h>        /* V4L2 的 ioctl 命令和结构体定义。 */
#include <stdio.h>                  /* printf / fprintf / perror。 */
#include <stdlib.h>                 /* calloc / free。 */
#include <string.h>                 /* memset。 */
#include <sys/ioctl.h>              /* ioctl。 */
#include <sys/mman.h>               /* mmap / munmap，用于映射摄像头缓冲区。 */
#include <unistd.h>                 /* close。 */

/* 单个 mmap 缓冲区的信息。 */
struct video_buffer {
    void *start;                    /* mmap 后得到的用户态虚拟地址。 */
    size_t length;                  /* 这个缓冲区的长度，后续 munmap 要用。 */
};

/* ioctl 包装函数：如果 ioctl 被信号中断，就自动重试。 */
static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;                        /* 保存 ioctl 返回值。 */

    do {                            /* ioctl 可能被信号打断。 */
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);

    return ret;                     /* 成功返回 0 或正数，失败返回 -1。 */
}

/* 把 V4L2 的 fourcc 整数转成人能看的字符串，比如 MJPG、YUYV。 */
static void fourcc_to_str(unsigned int fmt, char out[5])
{
    out[0] = (char)(fmt & 0xff);            /* fourcc 第 1 个字符。 */
    out[1] = (char)((fmt >> 8) & 0xff);     /* fourcc 第 2 个字符。 */
    out[2] = (char)((fmt >> 16) & 0xff);    /* fourcc 第 3 个字符。 */
    out[3] = (char)((fmt >> 24) & 0xff);    /* fourcc 第 4 个字符。 */
    out[4] = '\0';                          /* C 字符串结尾。 */
}

/* 打印摄像头能力：驱动名、设备名、总能力、支持的格式和分辨率。 */
int video_capture_print_capability(const char *device)
{
    int fd;                         /* 摄像头设备文件描述符。 */
    struct v4l2_capability cap;     /* VIDIOC_QUERYCAP 返回的能力信息。 */
    struct v4l2_fmtdesc fmt;        /* VIDIOC_ENUM_FMT 用于枚举像素格式。 */

    fd = open(device, O_RDWR);      /* 打开摄像头设备，这里不用非阻塞也可以。 */
    if (fd < 0) {                   /* 打不开一般是设备不存在或权限不够。 */
        perror("open camera");
        return -1;
    }

    memset(&cap, 0, sizeof(cap));   /* 清零结构体，避免未初始化字段干扰 ioctl。 */
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        return -1;
    }

    printf("device      : %s\n", device);          /* 设备节点。 */
    printf("driver      : %s\n", cap.driver);      /* 驱动名，UVC 摄像头通常是 uvcvideo。 */
    printf("card        : %s\n", cap.card);        /* 摄像头产品名。 */
    printf("bus         : %s\n", cap.bus_info);    /* USB 总线位置。 */
    printf("capabilities: 0x%08x\n", cap.capabilities);
    printf("device_caps : 0x%08x\n", cap.device_caps);
    printf("formats:\n");

    memset(&fmt, 0, sizeof(fmt));                   /* 从 index=0 开始枚举格式。 */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;         /* 只看视频采集格式。 */
    while (xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        struct v4l2_frmsizeenum size;               /* 枚举当前格式支持的分辨率。 */
        char fcc[5];                                /* 保存 fourcc 字符串。 */

        fourcc_to_str(fmt.pixelformat, fcc);
        printf("  %s  %s\n", fcc, fmt.description);

        memset(&size, 0, sizeof(size));             /* 每个格式的分辨率从 index=0 开始。 */
        size.pixel_format = fmt.pixelformat;
        while (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0) {
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                printf("    %ux%u\n", size.discrete.width, size.discrete.height);
            }
            size.index++;                           /* 继续下一个分辨率。 */
        }
        fmt.index++;                                /* 继续下一个像素格式。 */
    }

    close(fd);                                      /* 枚举完成后关闭设备。 */
    return 0;
}

/* 打开摄像头，并配置分辨率、像素格式和 mmap 缓冲区。 */
int video_capture_open(struct video_capture *cap, const char *device,
                       unsigned int width, unsigned int height,
                       unsigned int pixfmt)
{
    struct v4l2_capability capability;       /* 查询设备是否具备采集能力。 */
    struct v4l2_format format;               /* 设置采集格式。 */
    struct v4l2_requestbuffers req;          /* 申请驱动缓冲区。 */
    unsigned int i;                          /* 循环变量。 */

    memset(cap, 0, sizeof(*cap));            /* 初始化调用方传入的采集上下文。 */
    cap->fd = -1;                            /* -1 表示还没有成功打开设备。 */
    if (pthread_mutex_init(&cap->ioctl_mutex, NULL) != 0) {
        fprintf(stderr, "pthread_mutex_init capture failed\n");
        return -1;
    }
    cap->mutex_ready = 1;

    cap->fd = open(device, O_RDWR | O_NONBLOCK);
    if (cap->fd < 0) {
        perror("open camera");
        return -1;
    }

    memset(&capability, 0, sizeof(capability));
    if (xioctl(cap->fd, VIDIOC_QUERYCAP, &capability) < 0) {
        perror("VIDIOC_QUERYCAP");
        video_capture_close(cap);
        return -1;
    }

    memset(&format, 0, sizeof(format));              /* 准备设置摄像头输出格式。 */
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;       /* 普通视频采集。 */
    format.fmt.pix.width = width;                    /* 请求宽度，例如 640。 */
    format.fmt.pix.height = height;                  /* 请求高度，例如 480。 */
    format.fmt.pix.pixelformat = pixfmt;             /* 请求 MJPG。 */
    format.fmt.pix.field = V4L2_FIELD_ANY;           /* 交给驱动选择逐行/隔行。 */

    if (xioctl(cap->fd, VIDIOC_S_FMT, &format) < 0) {
        perror("VIDIOC_S_FMT");
        video_capture_close(cap);
        return -1;
    }

    if (format.fmt.pix.pixelformat != pixfmt) {      /* 驱动可能会改成别的格式。 */
        char got[5], want[5];
        fourcc_to_str(format.fmt.pix.pixelformat, got);
        fourcc_to_str(pixfmt, want);
        fprintf(stderr, "camera selected %s, want %s\n", got, want);
        video_capture_close(cap);
        return -1;
    }

    cap->width = format.fmt.pix.width;               /* 保存驱动实际确认的宽度。 */
    cap->height = format.fmt.pix.height;             /* 保存驱动实际确认的高度。 */
    cap->pixfmt = format.fmt.pix.pixelformat;        /* 保存驱动实际确认的格式。 */

    memset(&req, 0, sizeof(req));                    /* 申请摄像头采集缓冲区。 */
    req.count = 8;                                   /* 引用计数会延长占用时间，多申请几个缓冲。 */
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;                   /* 使用 mmap，避免频繁拷贝。 */

    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        video_capture_close(cap);
        return -1;
    }

    if (req.count < 2) {                             /* 至少需要 2 个缓冲才能稳定流式采集。 */
        fprintf(stderr, "not enough camera mmap buffers\n");
        video_capture_close(cap);
        return -1;
    }

    cap->buffers = calloc(req.count, sizeof(*cap->buffers));
    if (!cap->buffers) {
        perror("calloc buffers");
        video_capture_close(cap);
        return -1;
    }
    cap->buffer_count = req.count;                   /* 记录驱动实际给了几个缓冲。 */

    for (i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;                      /* 查询第 i 个缓冲的位置和大小。 */

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            video_capture_close(cap);
            return -1;
        }

        cap->buffers[i].length = buf.length;
        cap->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, cap->fd, buf.m.offset);
        if (cap->buffers[i].start == MAP_FAILED) {
            perror("mmap");
            cap->buffers[i].start = NULL;
            video_capture_close(cap);
            return -1;
        }
    }

    return 0;                                        /* 打开和缓冲区初始化完成。 */
}

/* 把所有 mmap 缓冲放入驱动队列，然后启动视频流。 */
int video_capture_start(struct video_capture *cap)
{
    enum v4l2_buf_type type;                 /* STREAMON 需要的缓冲类型。 */
    unsigned int i;                          /* 循环变量。 */

    for (i = 0; i < cap->buffer_count; i++) {
        struct v4l2_buffer buf;              /* 要入队的缓冲。 */

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

    pthread_mutex_lock(&cap->ioctl_mutex);
    if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) {
        pthread_mutex_unlock(&cap->ioctl_mutex);
            perror("VIDIOC_QBUF");
            return -1;
        }
    pthread_mutex_unlock(&cap->ioctl_mutex);
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }

    return 0;
}

/* 等待并取出一帧数据。调用者用完后必须调用 video_capture_put_frame 归还。 */
int video_capture_get_frame(struct video_capture *cap, struct video_frame *frame)
{
    struct v4l2_buffer buf;                  /* 驱动返回的缓冲描述。 */
    fd_set fds;                              /* select 等待集合。 */
    struct timeval tv;                       /* select 超时时间。 */
    int ret;                                 /* select 返回值。 */

    FD_ZERO(&fds);
    FD_SET(cap->fd, &fds);
    tv.tv_sec = 2;                           /* 最多等 2 秒，避免程序卡死。 */
    tv.tv_usec = 0;

    ret = select(cap->fd + 1, &fds, NULL, NULL, &tv);
    if (ret <= 0) {
        if (ret == 0)
            fprintf(stderr, "camera dequeue timeout\n");
        else if (errno != EINTR)
            perror("select camera");
        return -1;
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    pthread_mutex_lock(&cap->ioctl_mutex);
    if (xioctl(cap->fd, VIDIOC_DQBUF, &buf) < 0) {
        pthread_mutex_unlock(&cap->ioctl_mutex);
        perror("VIDIOC_DQBUF");
        return -1;
    }
    pthread_mutex_unlock(&cap->ioctl_mutex);

    if (buf.index >= cap->buffer_count) {    /* 防御驱动返回异常索引。 */
        fprintf(stderr, "invalid camera buffer index %u\n", buf.index);
        return -1;
    }

    frame->data = cap->buffers[buf.index].start;     /* 指向 MJPG 帧数据。 */
    frame->size = buf.bytesused;                     /* 本帧实际有效字节数。 */
    frame->index = buf.index;                        /* 保存索引，归还时要用。 */
    frame->sequence = buf.sequence;                  /* 摄像头帧序号，便于调试。 */
    return 0;
}

/* 把已经处理完的帧归还给驱动，让它继续复用这个缓冲采集。 */
int video_capture_put_frame(struct video_capture *cap, const struct video_frame *frame)
{
    struct v4l2_buffer buf;                  /* 要重新入队的缓冲。 */

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = frame->index;

    pthread_mutex_lock(&cap->ioctl_mutex);
    if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) {
        pthread_mutex_unlock(&cap->ioctl_mutex);
        perror("VIDIOC_QBUF return");
        return -1;
    }
    pthread_mutex_unlock(&cap->ioctl_mutex);

    return 0;
}

/* 停止摄像头流。即使失败也不影响后续关闭流程。 */
void video_capture_stop(struct video_capture *cap)
{
    enum v4l2_buf_type type;                 /* STREAMOFF 需要的缓冲类型。 */

    if (cap->fd >= 0) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(cap->fd, VIDIOC_STREAMOFF, &type);
    }
}

/* 释放 mmap 缓冲、关闭设备，并把上下文恢复到空状态。 */
void video_capture_close(struct video_capture *cap)
{
    unsigned int i;                          /* 循环释放每一个 mmap 缓冲。 */

    if (!cap)
        return;

    if (cap->buffers) {
        for (i = 0; i < cap->buffer_count; i++) {
            if (cap->buffers[i].start && cap->buffers[i].length)
                munmap(cap->buffers[i].start, cap->buffers[i].length);
        }
        free(cap->buffers);
    }

    if (cap->fd >= 0)
        close(cap->fd);
    if (cap->mutex_ready)
        pthread_mutex_destroy(&cap->ioctl_mutex);

    memset(cap, 0, sizeof(*cap));            /* 避免外部误用旧指针和旧 fd。 */
    cap->fd = -1;
}
