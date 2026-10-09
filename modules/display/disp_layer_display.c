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

typedef uint32_t u32;               /* sunxi_display2.h 在用户态需要 u32。 */
typedef int32_t s32;                /* sunxi_display2.h 在用户态需要 s32。 */

#include <sunxi_display2.h>         /* 全志 disp uapi 结构体和 ioctl 命令。 */

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

    return disp_do_cmd(disp->fd, DISP_LAYER_SET_CONFIG2, &config); /* 提交图层配置。 */
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
    disp_do_cmd(disp->fd, DISP_LAYER_SET_CONFIG2, &config); /* 提交关闭。 */
    usleep(20000);                  /* 等一帧左右，避免释放太快。 */
}

/* 关闭图层并释放 /dev/disp。 */
void disp_layer_display_close(struct disp_layer_display *disp)
{
    if (!disp || disp->fd < 0)      /* 未打开则不用处理。 */
        return;                     /* 直接返回。 */

    disp_layer_display_hide(disp);  /* 先显式隐藏图层。 */
    close(disp->fd);                /* 关闭 /dev/disp。 */
    disp->fd = -1;                  /* 标记 fd 无效。 */
}
