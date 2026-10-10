/*
 * disp_layer_display.c
 *
 * 使用全志 /dev/disp layer 显示 dmabuf。
 *
 * 目标链路：
 *   G2D 输出 ARGB8888 到 ION/dmabuf
 *   -> /dev/disp DISP_LAYER_SET_CONFIG2
 *   -> LCD 控制器直接扫描显示
 */

#include "disp_layer_display.h"     /* 本模块结构体和函数声明。 */

#include <fcntl.h>                  /* open。 */
#include <stdbool.h>                /* bool。 */
#include <stdint.h>                 /* uint32_t / int32_t。 */
#include <stdio.h>                  /* perror / printf。 */
#include <stdlib.h>                 /* getenv。 */
#include <string.h>                 /* memset。 */
#include <sys/ioctl.h>              /* ioctl。 */
#include <unistd.h>                 /* close / usleep。 */
#include <errno.h>
#include <linux/netlink.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

typedef uint32_t u32;               /* sunxi_display2.h 在用户态需要 u32。 */
typedef int32_t s32;                /* sunxi_display2.h 在用户态需要 s32。 */

#include <sunxi_display2.h>         /* 全志 disp uapi 结构体和 ioctl 命令。 */
static int disp_do_cmd(int fd, unsigned int cmd, void *config);

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Ignore queued/foreign events; wait for a real display boundary after the submit. */
static int wait_latched(struct disp_layer_display *disp, int64_t submitted_ns) /* 等到提交之后的显示边界，确认旧缓冲不再被当前图层引用。 */
{
    int64_t deadline = monotonic_ns() + 500000000LL; /* 音频/摄像头重开时允许瞬态调度延迟；仍必须收到真实 VSYNC，不回退猜测 sleep。 */
    while (monotonic_ns() < deadline) { /* 总等待期限固定，积压事件不能无限延长等待。 */
        struct pollfd pollfd = {disp->vsync_fd, POLLIN, 0};
        char buffer[4096];
        struct sockaddr_nl sender;
        socklen_t sender_size = sizeof(sender);
        ssize_t size;
        int remaining = (int)((deadline - monotonic_ns() + 999999) / 1000000); /* 向上取整剩余毫秒，poll 不超过固定截止时间。 */
        int ret = poll(&pollfd, 1, remaining > 0 ? remaining : 1); /* 等待内核事件，不用固定 sleep 猜测画面已经更新。 */
        if (ret < 0 && errno == EINTR) /* poll 被信号打断时重新计算剩余期限。 */
            continue;
        if (ret <= 0)
            break;
        size = recvfrom(disp->vsync_fd, buffer, sizeof(buffer), 0, /* 取得一条内核 uevent，同时核对发送者身份。 */
                        (struct sockaddr *)&sender, &sender_size);
        if (size < 0 && errno == ENOBUFS) { /* 硬件初始化期间 UI 套接字可能积压内核事件。 */
            fprintf(stderr, "disp vsync: event overflow, waiting for fresh boundary\n"); /* 丢事件不等于显示超时。 */
            continue; /* 丢弃旧事件后仍等待提交之后的真实 VSYNC，不能猜测已显示。 */
        }
        if (size < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (size <= 0)
            break;
        if (sender.nl_pid != 0) /* 只接受来自内核的 uevent。 */
            continue;
        for (size_t pos = 0; pos < (size_t)size;) {
            size_t len = strnlen(buffer + pos, (size_t)size - pos); /* uevent 由 NUL 分隔字段组成，长度检查避免越界。 */
            if (len == (size_t)size - pos)
                break;
            if (len > 7 && strncmp(buffer + pos, "VSYNC0=", 7) == 0 && /* 只识别屏幕 0 的厂商 VSYNC 字段。 */
                strtoll(buffer + pos + 7, NULL, 10) >= submitted_ns) /* 滤掉提交之前积压的 VSYNC，旧事件不能证明当前帧已锁存。 */
                return 0;
            pos += len + 1; /* 移动到下一个 NUL 分隔字段。 */
        }
    }
    errno = ETIMEDOUT; /* 只有截止时间内未收到有效边界才判为显示等待超时。 */
    perror("disp vsync wait");
    return -1;
}

static int submit_latched(struct disp_layer_display *disp, struct disp_layer_config2 *config)
{
    int64_t submitted_ns = monotonic_ns(); /* 记录提交前时间，后续边界必须不早于此时刻。 */
    if (disp_do_cmd(disp->fd, DISP_LAYER_SET_CONFIG2, config) < 0)
        return -1;
    return wait_latched(disp, submitted_ns); /* 成功返回后，调用者才可复用被替换的输出页。 */
}

/* 根据 VIDEO_RGB 选择 disp 图层格式，必须和 G2D 输出格式保持一致。 */
static int select_disp_rgb_format(const char **name)
{
    const char *mode = getenv("VIDEO_RGB"); /* 读取目标格式调试开关。 */

    if (mode && strcmp(mode, "abgr") == 0) { /* ABGR8888。 */
        *name = "ABGR8888";
        return DISP_FORMAT_ABGR_8888;
    }
    if (mode && strcmp(mode, "rgba") == 0) { /* RGBA8888。 */
        *name = "RGBA8888";
        return DISP_FORMAT_RGBA_8888;
    }
    if (mode && strcmp(mode, "bgra") == 0) { /* BGRA8888。 */
        *name = "BGRA8888";
        return DISP_FORMAT_BGRA_8888;
    }
    if (mode && strcmp(mode, "xrgb") == 0) { /* XRGB8888。 */
        *name = "XRGB8888";
        return DISP_FORMAT_XRGB_8888;
    }

    *name = "ARGB8888";             /* 默认沿用当前已出画面的格式。 */
    return DISP_FORMAT_ARGB_8888;
}

/* 提交一个 /dev/disp ioctl 命令。 */
static int disp_do_cmd(int fd, unsigned int cmd, void *config)
{
    unsigned long args[4];          /* 全志 disp ioctl 使用的参数数组。 */

    memset(args, 0, sizeof(args));  /* 清空参数。 */
    args[0] = 0;                    /* display id，LCD0 使用 0。 */
    args[1] = (unsigned long)config;/* 指向配置结构体。 */
    args[2] = 1;                    /* 配置数量。 */

    if (ioctl(fd, cmd, args) < 0) { /* 提交 ioctl。 */
        perror("disp ioctl");      /* 打印系统错误。 */
        return -1;                  /* 返回失败。 */
    }
    return 0;                       /* 返回成功。 */
}

/* 打开 /dev/disp 并准备一个显示图层。 */
int disp_layer_display_open(struct disp_layer_display *disp,
                            unsigned int screen_width,
                            unsigned int screen_height)
{
    return disp_layer_display_open_ex(disp, screen_width, screen_height,
                                      1, 0, 11, 0, 0,
                                      screen_width, screen_height);
}

/* 打开 /dev/disp，并准备一个指定位置和层级的显示图层。 */
int disp_layer_display_open_ex(struct disp_layer_display *disp,
                               unsigned int screen_width,
                               unsigned int screen_height,
                               unsigned int channel,
                               unsigned int layer_id,
                               unsigned int zorder,
                               int window_x,
                               int window_y,
                               unsigned int window_width,
                               unsigned int window_height)
{
    memset(disp, 0, sizeof(*disp)); /* 清空上下文。 */
    disp->fd = -1;                  /* fd 默认无效。 */
    disp->vsync_fd = -1; /* 无效事件句柄不参与后续等待。 */
    disp->screen_width = screen_width; /* 保存屏幕宽度。 */
    disp->screen_height = screen_height; /* 保存屏幕高度。 */
    disp->channel = channel;        /* 设置显示通道。 */
    disp->layer_id = layer_id;      /* 设置图层号。 */
    disp->zorder = zorder;          /* 设置图层叠放顺序。 */
    disp->window_x = window_x;      /* 保存窗口 x。 */
    disp->window_y = window_y;      /* 保存窗口 y。 */
    disp->window_width = window_width; /* 保存窗口宽度。 */
    disp->window_height = window_height; /* 保存窗口高度。 */

    disp->fd = open("/dev/disp", O_RDWR); /* 打开全志显示设备。 */
    if (disp->fd < 0) {             /* 打开失败。 */
        perror("/dev/disp");        /* 打印错误。 */
        return -1;                  /* 返回失败。 */
    }
    {
        struct sockaddr_nl address = {0};
        unsigned long args[4] = {0, 1, 0, 0};
        disp->vsync_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, /* 建立每个显示上下文自己的非阻塞事件套接字。 */
                                NETLINK_KOBJECT_UEVENT);
        address.nl_family = AF_NETLINK; /* 连接 Linux 内核事件通道。 */
        address.nl_groups = 1; /* 订阅 kobject uevent 组，包含厂商 VSYNC 事件。 */
        if (disp->vsync_fd < 0 ||
            bind(disp->vsync_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
            ioctl(disp->fd, DISP_VSYNC_EVENT_EN, args) < 0) { /* 开启屏幕 0 的 VSYNC 通知，失败就不允许猜测双缓冲同步。 */
            perror("disp vsync setup");
            if (disp->vsync_fd >= 0)
                close(disp->vsync_fd); /* 释放事件订阅句柄，不再等待显示通知。 */
            close(disp->fd);
            disp->fd = disp->vsync_fd = -1;
            return -1;
        }
    }

    printf("disp layer  : channel=%u layer=%u z=%u screen=%ux%u window=%d,%d %ux%u\n",
           disp->channel, disp->layer_id,
           disp->zorder,
           disp->screen_width, disp->screen_height,
           disp->window_x, disp->window_y,
           disp->window_width, disp->window_height); /* 打印图层信息。 */
    return 0;                       /* 打开成功。 */
}

/* 把一个 ARGB8888 dmabuf 显示到 LCD 图层。 */
int disp_layer_display_show_argb8888(struct disp_layer_display *disp,
                                     int dmabuf_fd,
                                     unsigned int width,
                                     unsigned int height)
{
    struct disp_layer_config2 config; /* 全志 disp layer v2 配置。 */
    const char *format_name;          /* 图层格式名称，仅用于首次日志。 */
    int disp_format;                  /* 图层格式。 */

    if (!disp || disp->fd < 0 || dmabuf_fd < 0) /* 检查关键参数。 */
        return -1;                  /* 参数无效。 */

    disp_format = select_disp_rgb_format(&format_name); /* 与 G2D 输出保持一致。 */

    memset(&config, 0, sizeof(config)); /* 清空配置结构体。 */
    config.channel = disp->channel; /* 设置显示通道。 */
    config.layer_id = disp->layer_id; /* 设置图层号。 */
    config.enable = 1;              /* 启用图层。 */

    config.info.mode = LAYER_MODE_BUFFER; /* 图层内容来自 buffer。 */
    config.info.zorder = disp->zorder; /* 设置图层 zorder。 */
    config.info.alpha_mode = 1;     /* 使用全局 alpha。 */
    config.info.alpha_value = 0xff; /* 完全不透明。 */
    config.info.screen_win.x = disp->window_x; /* 显示窗口 x。 */
    config.info.screen_win.y = disp->window_y; /* 显示窗口 y。 */
    config.info.screen_win.width = disp->window_width; /* 显示窗口宽度。 */
    config.info.screen_win.height = disp->window_height; /* 显示窗口高度。 */

    config.info.fb.fd = dmabuf_fd;  /* 把 G2D 输出 dmabuf 交给 disp。 */
    config.info.fb.size[0].width = width; /* buffer 宽度。 */
    config.info.fb.size[0].height = height; /* buffer 高度。 */
    config.info.fb.format = disp_format; /* G2D 输出格式。 */
    config.info.fb.color_space = DISP_BT709; /* RGB 图层色彩空间填 BT709。 */
    config.info.fb.crop.x = 0;      /* crop 使用 32.32 定点格式。 */
    config.info.fb.crop.y = 0;      /* crop 起点 y。 */
    config.info.fb.crop.width = (unsigned long long)width << 32; /* 源宽。 */
    config.info.fb.crop.height = (unsigned long long)height << 32; /* 源高。 */

    {
        static int printed_once;    /* 只打印一次，避免实时预览刷屏。 */
        if (!printed_once) {        /* 第一次显示时输出图层格式。 */
            printf("disp format : %s\n", format_name);
            printed_once = 1;       /* 后续不再重复打印。 */
        }
    }

    return submit_latched(disp, &config); /* 提交新图层配置并等待真实锁存边界。 */
}

/* 只隐藏图层，不关闭 /dev/disp fd。 */
void disp_layer_display_hide(struct disp_layer_display *disp)
{
    struct disp_layer_config2 config; /* 用于关闭图层的配置。 */

    if (!disp || disp->fd < 0)      /* 未打开则不用处理。 */
        return;                     /* 直接返回。 */

    memset(&config, 0, sizeof(config)); /* 清空配置。 */
    config.channel = disp->channel; /* 指定通道。 */
    config.layer_id = disp->layer_id; /* 指定图层。 */
    config.enable = 0;              /* 关闭图层。 */
    submit_latched(disp, &config); /* Observe a real boundary before releasing old dmabufs. */
}

/* 关闭图层并释放 /dev/disp。 */
void disp_layer_display_close(struct disp_layer_display *disp)
{
    if (!disp || disp->fd < 0)      /* 未打开则不用处理。 */
        return;                     /* 直接返回。 */

    disp_layer_display_hide(disp);  /* 先显式隐藏图层。 */
    close(disp->fd);                /* 关闭 /dev/disp。 */
    if (disp->vsync_fd >= 0)
        close(disp->vsync_fd); /* 释放事件订阅句柄，不再等待显示通知。 */
    disp->vsync_fd = -1; /* 无效事件句柄不参与后续等待。 */
    disp->fd = -1;                  /* 标记 fd 无效。 */
}
