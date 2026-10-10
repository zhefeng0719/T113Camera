/*
 * main.c
 *
 * T113Camera 第一版整合入口。
 *
 * 运行方式：
 *   ./t113_camera        播放列表与三按键交互，按 Ctrl+C 退出。
 *   ./t113_camera 5      定时录制 5 秒 MJPEG + PCM AVI，同时屏幕预览。
 */

#include "disp_layer_display.h"     /* /dev/disp layer 显示接口。 */
#include "ffmpeg_audio_player.h"    /* FFmpeg AAC/MP3/PCM 音频播放接口。 */
#include "ffmpeg_mjpeg_demuxer.h"   /* FFmpeg MJPEG 容器解封装接口。 */
#include "ffmpeg_mjpeg_muxer.h"     /* FFmpeg MJPEG/PCM AVI 封装接口。 */
#include "g2d_display.h"            /* G2D YUV->RGB + 缩放显示接口。 */
#include "image_convert.h"          /* CedarC MJPG->YUV 解码接口。 */
#include "jpeg_info.h"
#include "input_key.h"              /* 三个用户按键输入接口。 */
#include "media_list.h"             /* U 盘媒体文件扫描接口。 */
#include "media_storage.h"          /* 挂载检查、自动命名和 JPEG 原子保存。 */
#include "storage_monitor.h"        /* 独立存储监控，只传状态和取消信号。 */
#include "record_writer.h"
#include "playback_queue.h"         /* 播放读取线程和显示线程之间的 packet 队列。 */
#include "t113_ui_lvgl.h"           /* LVGL GUI Guider 界面显示接口。 */
#include "audio_capture.h"          /* ALSA PCM 音频采集队列。 */
#include "audio_alsa.h"             /* ALSA PCM 播放接口。 */
#include "media_clock.h"
#include "wav_player.h"             /* PCM WAV 文件播放接口。 */
#include "video_capture.h"          /* V4L2 摄像头采集接口。 */

#include <errno.h>                  /* errno。 */
#include <fcntl.h>
#include <limits.h>                 /* LONG_MIN / LONG_MAX。 */
#include <linux/videodev2.h>        /* V4L2_PIX_FMT_MJPEG。 */
#include <pthread.h>                /* pthread 线程、互斥锁、条件变量。 */
#include <signal.h>                 /* signal / sig_atomic_t。 */
#include <stdio.h>                  /* printf / fprintf / perror。 */
#include <stdlib.h>                 /* strtol。 */
#include <string.h>                 /* memset。 */
#include <sys/wait.h>               /* WIFEXITED / WEXITSTATUS。 */
#include <sys/file.h>
#include <time.h>                   /* clock_gettime。 */
#include <unistd.h>                 /* usleep。 */

#define DEFAULT_DEVICE "/dev/video0"/* USB UVC 摄像头采集节点。 */
#define DEFAULT_WIDTH 640           /* 摄像头采集宽度。 */
#define DEFAULT_HEIGHT 480          /* 摄像头采集高度。 */
#define USB_ROOT g_storage_root     /* 实际挂载路径；只由应用主线程更新。 */
#define INPUT_DEVICE "/dev/input/event1" /* 当前已验证的三个用户按键节点。 */
#define AUDIO_QUEUE_CAPACITY 64     /* Bounded PCM backlog: about 1.36 seconds at 48 kHz. */
#define PLAYER_FPS 60U              /* MJPEG 裸流无时间戳，按 60fps 作为目标显示节奏。 */
#define CONTAINER_VIDEO_FPS 30U     /* 当前 MP4/AVI 容器视频先按 30fps 显示。 */

enum playback_result {
    PLAYBACK_RESULT_ERROR = -1,     /* 播放过程出错。 */
    PLAYBACK_RESULT_END = 0,        /* 正常播放到文件结束。 */
    PLAYBACK_RESULT_NEXT = 1,       /* 播放中短按 NEXT，停止播放并切换下一项。 */
    PLAYBACK_RESULT_PREV = 2,       /* 播放中短按 ENTER，停止播放并切换上一项。 */
    PLAYBACK_RESULT_RECORD = 3,     /* 长按中键：先关闭播放，再进入相机。 */
    PLAYBACK_RESULT_STORAGE_LOST = 4, /* 存储会话取消，不得误报为文件正常播放完毕。 */
};

static const char *const g_record_names[] = { /* 录制模式左侧可选文件名。 */
    "record1",
    "record2",
    "record3",
    "record4",
    "record5",
};

static volatile sig_atomic_t g_stop;/* Ctrl+C 信号会把它置 1，主循环据此退出。 */
static struct storage_monitor g_storage; /* 应用拥有监控线程，模块之间不互相调用。 */
static char g_storage_root[512];    /* 当前确认的挂载路径；缺盘时为空，禁止落盘到 flash。 */
static int g_storage_enabled;      /* 不使用 U 盘的独立显示命令无需监控。 */

static int storage_session_lost(void)
{
    return g_storage_enabled && storage_monitor_cancelled(&g_storage); /* 播放/录制循环共用失效条件。 */
}

static void storage_bind_task(volatile int *stop)
{
    if (g_storage_enabled) storage_monitor_bind(&g_storage, stop); /* 绑定和解绑保证栈变量生命周期。 */
}

struct fps_meter;                  /* 前置声明：FPS 辅助函数参数要用。 */
static void kill_lvgl_demo(void);   /* 前置声明：run_ui 也要停掉开机 demo。 */
static void fps_meter_reset(struct fps_meter *meter);
static void fps_meter_frame(struct fps_meter *meter);
static uint8_t record_breath_brightness(void);

/* MJPEG 裸流读取线程上下文。 */
struct mjpeg_source_worker {
    struct playback_packet_queue *queue; /* 输出队列。 */
    const unsigned char *data;      /* 文件内存。 */
    size_t size;                    /* 文件大小。 */
    int ret;                        /* 线程结果。 */
};

/* MKV 解封装读取线程上下文。 */
struct mkv_source_worker {
    struct playback_packet_queue *queue; /* 输出队列。 */
    struct ffmpeg_mjpeg_demuxer *demuxer; /* 已打开的 FFmpeg demuxer。 */
    int ret;                        /* 线程结果。 */
};

/* AVI 音频播放线程上下文。 */
struct avi_audio_worker {
    struct media_clock *clock;      /* 音频硬件播放位置供视频调度读取。 */
    int done;                      /* release/acquire 发布线程结束和结果。 */
    const char *path;               /* 当前 AVI 文件路径。 */
    volatile int *stop;             /* 主线程退出播放时置 1。 */
    volatile int *paused;           /* 主线程暂停播放时置 1。 */
    int ret;                        /* 线程结果。 */
};

/* WAV 播放线程上下文。 */
struct wav_play_worker {
    const char *path;               /* 当前 WAV 文件路径。 */
    volatile int stop;              /* 主线程要求停止时置 1。 */
    volatile int paused;            /* 主线程要求暂停时置 1。 */
    volatile int done;              /* 线程结束时置 1。 */
    volatile uint32_t played_ms;    /* 音频线程更新的已播放时长。 */
    volatile uint32_t total_ms;     /* 音频线程更新的总时长。 */
    int ret;                        /* 播放结果。 */
};

/* 录制模式预览上下文：把摄像头实时画面显示到 UI 的 vedioplay 区域。 */
struct record_preview {
    int opened;                     /* 资源是否已经打开。 */
    int recording;                  /* 当前是否正在把 MJPG 帧写入 U 盘。 */
    struct video_capture cap;        /* V4L2 摄像头。 */
    struct disp_layer_display disp;  /* vedioplay 区域的视频图层。 */
    struct g2d_display g2d;          /* G2D 缩放/格式转换。 */
    struct cedar_decoder decoder;    /* CedarC MJPG 解码器。 */
    struct ffmpeg_mjpeg_muxer writer; /* 录制模式使用的 MKV 封装器。 */
    struct record_writer sink;       /* USB writes never run on the preview/UI thread. */
    struct audio_queue audio_queue;  /* 录制模式音频 PCM 队列。 */
    struct audio_capture_worker audio_worker; /* ALSA 音频采集线程参数。 */
    pthread_t audio_thread;          /* ALSA 音频采集线程。 */
    char record_path[512];           /* 当前录制文件路径。 */
    unsigned int frames;             /* 已显示帧数。 */
    int audio_queue_ready;           /* 音频队列是否已初始化。 */
    int audio_started;               /* 音频采集线程是否已启动。 */
};

/* 播放列表第一帧预览上下文：保持 video layer，不让预览图函数退出后马上 hide。 */
struct still_preview {
    int opened;                     /* 预览图层是否已打开。 */
    struct disp_layer_display disp; /* vedioplay 区域的视频图层。 */
    struct g2d_display g2d;         /* G2D 缩放/格式转换。 */
    struct cedar_decoder decoder;   /* CedarC MJPG 解码器。 */
};

/* FPS 统计器：按 1 秒窗口计算实际显示帧率。 */
struct fps_meter {
    struct timespec start;          /* 当前统计窗口开始时间。 */
    unsigned int frames;            /* 当前窗口累计帧数。 */
};

/* Ctrl+C 信号处理函数：只设置标志，不在信号里做复杂清理。 */
static void on_signal(int signo)
{
    (void)signo;                    /* 当前不关心具体信号编号。 */
    g_stop = 1;                     /* 通知主循环自然退出。 */
}

/* 打印程序用法。 */
static void print_usage(const char *prog)
{
    printf("usage:\n");             /* 打印用法标题。 */
    printf("  %s\n", prog);         /* 无参数：播放列表模式。 */
    printf("  %s preview\n", prog); /* preview：实时预览。 */
    printf("  %s ui\n", prog);      /* ui：显示 GUI Guider 界面。 */
    printf("  %s ui <video.mjpeg|video.avi>\n", prog); /* ui + 文件：在 UI 中播放视频。 */
    printf("  %s <1-59>\n", prog);  /* 一个参数：录制秒数。 */
    printf("\n");                   /* 空行让终端输出更清楚。 */
    printf("player : USB media list + keys from %s\n", INPUT_DEVICE);
    printf("preview: UVC MJPG -> CedarC NV21 -> G2D -> /dev/disp layer\n");
    printf("record : UVC MJPG -> %s/T113Camera/VIDEO/video.avi\n", USB_ROOT);
}

/* 一次性读入一个文件，当前用于第一版 MJPEG 裸流播放。 */
static int read_file_all(const char *path, unsigned char **data, size_t *size) /* 照片与裸 MJPEG 的整文件读取辅助；容器播放使用流式解封装。 */
{
    FILE *fp;                       /* 文件句柄。 */
    long length;                    /* 文件长度。 */
    unsigned char *buffer;          /* 文件内容缓冲。 */

    *data = NULL;                   /* 默认无数据。 */
    *size = 0;                      /* 默认长度为 0。 */

    fp = fopen(path, "rb");        /* 打开媒体文件。 */
    if (fp == NULL) {               /* 打开失败。 */
        perror("fopen video");      /* 打印错误。 */
        return -1;                  /* 返回失败。 */
    }

    if (fseek(fp, 0, SEEK_END) < 0) { /* 移到文件末尾。 */
        perror("fseek end");       /* 打印错误。 */
        fclose(fp);                 /* 关闭文件。 */
        return -1;                  /* 返回失败。 */
    }
    length = ftell(fp);             /* 读取文件长度。 */
    if (length <= 0 || length > 16L * 1024 * 1024) { /* 小内存板限制整文件载入，容器播放不受影响。 */
        fprintf(stderr, "invalid video file size\n");
        fclose(fp);                 /* 关闭文件。 */
        return -1;                  /* 返回失败。 */
    }
    if (fseek(fp, 0, SEEK_SET) < 0) { /* 回到文件开头。 */
        perror("fseek set");       /* 打印错误。 */
        fclose(fp);                 /* 关闭文件。 */
        return -1;                  /* 返回失败。 */
    }

    buffer = malloc((size_t)length); /* 分配文件缓冲。 */
    if (buffer == NULL) {           /* 分配失败。 */
        fprintf(stderr, "malloc video buffer failed, size=%ld\n", length);
        fclose(fp);                 /* 关闭文件。 */
        return -1;                  /* 返回失败。 */
    }

    size_t offset = 0; /* 分块读取，块与块之间检查停止信号。 */
    while (offset < (size_t)length && !g_stop && !storage_session_lost()) { /* 不能强制打断单次内核 read。 */
        size_t chunk = (size_t)length - offset; /* 当前剩余字节数。 */
        if (chunk > 65536) chunk = 65536; /* 每次最多读取 64 KiB。 */
        size_t count = fread(buffer + offset, 1, chunk, fp); /* 有界文件读取。 */
        offset += count; /* 累加实际读取长度。 */
        if (count != chunk) break; /* EOF 或错误均停止读取。 */
    }
    if (offset != (size_t)length || storage_session_lost()) { /* 不交付失效会话的图像。 */
        perror("fread video");     /* 打印错误。 */
        free(buffer);               /* 释放缓冲。 */
        fclose(fp);                 /* 关闭文件。 */
        return -1;                  /* 返回失败。 */
    }

    fclose(fp);                     /* 关闭文件。 */
    *data = buffer;                 /* 输出缓冲指针。 */
    *size = (size_t)length;         /* 输出文件长度。 */
    return 0;                       /* 读取成功。 */
}

/* 简单判断路径后缀，命令行测试入口用它区分裸流和 MKV。 */
static int path_ends_with_ci(const char *path, const char *suffix)
{
    size_t path_len;                /* 路径长度。 */
    size_t suffix_len;              /* 后缀长度。 */
    const char *tail;               /* 路径尾部。 */

    if (path == NULL || suffix == NULL) /* 参数检查。 */
        return 0;                   /* 无效参数视为不匹配。 */
    path_len = strlen(path);        /* 计算路径长度。 */
    suffix_len = strlen(suffix);    /* 计算后缀长度。 */
    if (suffix_len > path_len)      /* 后缀比路径还长。 */
        return 0;                   /* 不可能匹配。 */

    tail = path + path_len - suffix_len; /* 定位路径尾部。 */
    while (*tail != '\0' && *suffix != '\0') { /* 逐字符比较。 */
        char a = *tail++;           /* 路径字符。 */
        char b = *suffix++;         /* 后缀字符。 */

        if (a >= 'A' && a <= 'Z')   /* 手写小写转换，避免额外依赖。 */
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')   /* 手写小写转换。 */
            b = (char)(b - 'A' + 'a');
        if (a != b)                 /* 有一个字符不同。 */
            return 0;               /* 不匹配。 */
    }

    return 1;                       /* 后缀匹配。 */
}

/* 从 MJPEG 裸流中寻找下一帧 JPEG，帧边界使用 SOI=FFD8 和 EOI=FFD9。 */
static int find_next_jpeg_frame(const unsigned char *data,
                                size_t size,
                                size_t *offset,
                                const unsigned char **frame,
                                size_t *frame_size)
{
    return jpeg_next_frame(data, size, offset, frame, frame_size); /* 按 JPEG 段结构找完整帧，不能搜索第一个 0xffd9 就截断 EXIF 主图。 */
}

/* 统计 MJPEG 裸流中完整 JPEG 帧数量，用来把进度条和实际播放帧数挂钩。 */
static unsigned int count_mjpeg_frames(const unsigned char *data, size_t size)
{
    size_t offset = 0;              /* 从文件开头开始扫描。 */
    unsigned int count = 0;         /* 找到的完整 JPEG 帧数。 */

    while (1) {                     /* 一直扫到没有完整帧。 */
        const unsigned char *frame; /* 当前帧起点。 */
        size_t frame_size;          /* 当前帧大小。 */

        if (!find_next_jpeg_frame(data, size, &offset, &frame, &frame_size))
            break;                  /* 没有更多完整帧。 */
        (void)frame;                /* 这里只统计，不使用帧指针。 */
        (void)frame_size;           /* 这里只统计，不使用帧大小。 */
        count++;                    /* 统计一帧。 */
    }

    return count;                   /* 返回总帧数。 */
}

/* 把帧数换算成毫秒，用于让播放进度条和播放时长挂钩。 */
static uint32_t frames_to_ms(unsigned int frames)
{
    return (uint32_t)(((unsigned long long)frames * 1000ULL) / PLAYER_FPS); /* 裸 MJPEG 无 PTS，只能按约定帧率估算时长；AVI 不使用此估算。 */
}

/* 更新播放进度条，value/max 都使用毫秒单位。 */
static void set_playback_progress(unsigned int frames, unsigned int total_frames)
{
    uint32_t played_ms;             /* 已播放时长。 */
    uint32_t total_ms;              /* 总播放时长。 */

    total_ms = frames_to_ms(total_frames); /* 总帧数换算成总时长。 */
    if (total_ms == 0U)             /* slider 的 max 不能为 0。 */
        total_ms = 1U;              /* 极短文件兜底。 */

    played_ms = frames_to_ms(frames); /* 已播放帧数换算成已播放时长。 */
    if (played_ms > total_ms)       /* 防止末尾越界。 */
        played_ms = total_ms;       /* 限制到总时长。 */

    t113_ui_lvgl_set_progress(played_ms, total_ms); /* 交给 LVGL slider。 */
}

/* 直接用毫秒更新播放进度，给 MKV 这种带时间戳的容器使用。 */
static void set_playback_progress_ms(int64_t played_ms, int64_t total_ms)
{
    if (total_ms <= 0)              /* 没拿到总时长时给一个兜底值。 */
        total_ms = 1;               /* slider 的 max 不能为 0。 */
    if (played_ms < 0)              /* 没拿到 PTS 时从 0 开始显示。 */
        played_ms = 0;              /* 防止负数传给无符号 UI。 */
    if (played_ms > total_ms)       /* 防止末尾越界。 */
        played_ms = total_ms;       /* 限制到总时长。 */

    t113_ui_lvgl_set_progress((uint32_t)played_ms, (uint32_t)total_ms);
}

/* WAV 播放器的进度回调：把已播放毫秒数映射到 UI slider。 */
static void wav_progress_cb(uint32_t played_ms, uint32_t total_ms, void *user)
{
    struct wav_play_worker *worker = user; /* WAV 播放线程上下文。 */

    if (worker == NULL)
        return;
    __atomic_store_n(&worker->played_ms, played_ms, __ATOMIC_RELEASE); /* 音频线程发布进度，控制线程通过 acquire 读取。 */
    __atomic_store_n(&worker->total_ms, total_ms, __ATOMIC_RELEASE); /* 跨线程发布总时长，不从音频线程直接操作 LVGL。 */
}

/* 给 timespec 增加指定毫秒，供绝对时间调度使用。 */
static struct timespec timespec_add_ms(struct timespec base, int64_t ms)
{
    base.tv_sec += (time_t)(ms / 1000); /* 秒部分直接相加。 */
    base.tv_nsec += (long)((ms % 1000) * 1000000LL); /* 毫秒转纳秒。 */
    while (base.tv_nsec >= 1000000000L) { /* 处理纳秒进位。 */
        base.tv_nsec -= 1000000000L; /* 纳秒减一秒。 */
        base.tv_sec++;              /* 秒数进位。 */
    }
    return base;                    /* 返回目标时间点。 */
}

/* 给 timespec 减去指定毫秒，用于暂停恢复后重建调度起点。 */
static struct timespec timespec_sub_ms(struct timespec base, int64_t ms)
{
    base.tv_sec -= (time_t)(ms / 1000); /* 秒部分相减。 */
    base.tv_nsec -= (long)((ms % 1000) * 1000000LL); /* 纳秒部分相减。 */
    while (base.tv_nsec < 0) {      /* 处理借位。 */
        base.tv_nsec += 1000000000L; /* 纳秒补一秒。 */
        base.tv_sec--;              /* 秒数借位。 */
    }
    return base;                    /* 返回结果。 */
}

/* 睡到绝对时间点；如果已经超时则立即返回。 */
static void sleep_until_monotonic(struct timespec target)
{
    struct timespec now;            /* 当前时间。 */
    int64_t diff_ns;                /* 距离目标的纳秒数。 */
    int ret;                        /* clock_nanosleep 返回值。 */

    clock_gettime(CLOCK_MONOTONIC, &now); /* 使用单调时钟避免系统时间变化。 */
    diff_ns = (int64_t)(target.tv_sec - now.tv_sec) * 1000000000LL +
              (int64_t)(target.tv_nsec - now.tv_nsec); /* 计算剩余时间。 */
    if (diff_ns <= 0)               /* 已经晚了。 */
        return;                     /* 不 sleep，尽快追帧。 */
    if (diff_ns > 100000000LL)      /* 防止异常 PTS 让画面长时间停住。 */
        diff_ns = 100000000LL;      /* 单次最多等待 100ms。 */
    do {
        ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, /* 等待绝对截止时刻，避免逐帧相对 sleep 累积执行耗时。 */
                              &target, NULL); /* 绝对时间睡眠，避免累计误差。 */
    } while (ret == EINTR);         /* 被信号打断时继续睡到同一个目标时间。 */
}

/* 按固定帧率或容器时间戳计算下一帧显示时间。 */
static int64_t playback_due_ms(unsigned int frame_index, int64_t pts_ms)
{
    if (pts_ms >= 0)                /* MKV 优先使用容器 PTS。 */
        return pts_ms;              /* PTS 本身就是从 0 开始的毫秒。 */
    return (int64_t)(((unsigned long long)frame_index * 1000ULL) / PLAYER_FPS); /* 仅缺失时间戳时以帧序号和默认帧率退化调度。 */
}

/* 如果 LVGL 有新内容刷新，就重新提交 UI 图层。 */
static int submit_ui_if_dirty(struct disp_layer_display *disp,
                              struct t113_ui_lvgl *ui,
                              uint32_t *last_flush)
{
    uint32_t flush_now;             /* 当前 LVGL flush 次数。 */
    if (ui->present_failed)
        return -1;

    t113_ui_lvgl_tick(ui);          /* 让 LVGL 处理一次定时器。 */
    flush_now = t113_ui_lvgl_get_flush_count(ui); /* 读取刷新计数。 */
    if (flush_now == *last_flush)   /* 没变化就不重复提交图层。 */
        return 0;                   /* 没有新画面。 */

    *last_flush = flush_now;        /* 更新已提交计数。 */
    return t113_ui_lvgl_present(ui, disp);
}

/* 强制提交一次 UI 图层，用于模式切换后立刻覆盖旧画面。 */
static int submit_ui_force(struct disp_layer_display *disp,
                           struct t113_ui_lvgl *ui,
                           uint32_t *last_flush)
{
    t113_ui_lvgl_tick(ui);          /* 先让 LVGL 处理刚设置的文字/列表。 */
    *last_flush = t113_ui_lvgl_get_flush_count(ui); /* 同步刷新计数。 */
    return t113_ui_lvgl_present(ui, disp);
}

/* 关闭用于视频叠加的 channel=1 layer=1，避免切换模式后旧视频盖住 UI。 */
static void hide_video_overlay_layer(void)
{
    struct disp_layer_display video_disp; /* 临时上下文，只用于提交 disable。 */

    memset(&video_disp, 0, sizeof(video_disp)); /* 清空上下文。 */
    if (disp_layer_display_open_ex(&video_disp, 800, 480,
                                   1, 1, 12,
                                   0, 0, 800, 480) == 0) {
        disp_layer_display_hide(&video_disp); /* 显式隐藏视频层。 */
        disp_layer_display_close(&video_disp); /* 关闭临时 fd。 */
    }
}

/* 把媒体扫描结果填到 GUI Guider 的 playlist 控件里。 */
static void fill_playlist_ui(const struct media_list *list, size_t selected)
{
    size_t i;                       /* 列表下标。 */

    t113_ui_lvgl_playlist_clear();  /* 先清空旧列表。 */
    if (list == NULL || list->count == 0) { /* 没有媒体文件。 */
        t113_ui_lvgl_playlist_add("EMPTY");
        t113_ui_lvgl_playlist_select(0);
        return;
    }

    for (i = 0; i < list->count; i++) { /* 逐个添加文件名。 */
        char line[192];             /* UI 中显示的一行文本。 */
        const struct media_item *item = &list->items[i]; /* 当前媒体条目。 */

        snprintf(line, sizeof(line), "%s  %s",
                 media_type_name(item->type), item->name); /* 类型 + 文件名。 */
        t113_ui_lvgl_playlist_add(line); /* 添加到 LVGL list。 */
    }

    t113_ui_lvgl_playlist_select((uint32_t)selected); /* 高亮当前项。 */
}

/* 扫描 U 盘媒体并刷新播放模式列表。 */
static int refresh_media_list_ui(struct media_list *list, size_t *selected)
{
    if (!storage_session_lost() && *USB_ROOT && media_list_scan(list, USB_ROOT) == 0) { /* 只扫描已确认的会话。 */
        char usb_text[128];         /* U 盘状态文字。 */

        snprintf(usb_text, sizeof(usb_text), "USB: %s", USB_ROOT);
        t113_ui_lvgl_set_usb_status(usb_text);
    } else {
        media_list_init(list);      /* 拔出后清空旧路径，禁止再次选择旧文件。 */
        t113_ui_lvgl_set_usb_status("USB NOT READY");
    }

    if (selected != NULL && list->count > 0 && *selected >= list->count)
        *selected = list->count - 1U; /* 删除或重扫后防止选中项越界。 */
    if (selected != NULL && list->count == 0)
        *selected = 0;              /* 空列表固定到 0。 */

    fill_playlist_ui(list, selected != NULL ? *selected : 0U);
    return list->count > 0 ? 0 : -1; /* 没有文件也算扫描完成，但给调用者一个空列表提示。 */
}

/* 把 record1..record5 填到 GUI Guider 的 playlist 控件里。 */
static void fill_record_names_ui(size_t selected)
{
    size_t i;                       /* 录制文件名下标。 */
    size_t count = sizeof(g_record_names) / sizeof(g_record_names[0]); /* 文件名数量。 */

    t113_ui_lvgl_playlist_clear();  /* 录制模式不显示媒体歌单。 */
    for (i = 0; i < count; i++)     /* 逐个添加可选录制文件名。 */
        t113_ui_lvgl_playlist_add(g_record_names[i]);
    t113_ui_lvgl_playlist_select((uint32_t)(selected % count)); /* 高亮当前保存名。 */
}

/* 更新播放模式标题和视频提示文字。 */
static void show_selected_media_hint(const struct media_list *list, size_t selected)
{
    const struct media_item *item;  /* 当前条目。 */

    if (list == NULL || list->count == 0) { /* 没文件。 */
        t113_ui_lvgl_set_audio_visualizer(false);
        t113_ui_lvgl_set_title("No media", "Insert U disk files");
        t113_ui_lvgl_set_video_message("NO FILE");
        return;
    }

    item = &list->items[selected];  /* 取当前高亮项。 */
    t113_ui_lvgl_set_title(item->name, media_type_name(item->type));
    if (item->type == MEDIA_TYPE_WAV)
        t113_ui_lvgl_set_video_message("AUDIO");
    else {
        t113_ui_lvgl_set_audio_visualizer(false);
        t113_ui_lvgl_set_video_message(item->playable ? "" : "NO SUPPORT");
    }
}

/* 关闭播放列表第一帧预览图层。 */
static void still_preview_close(struct still_preview *preview)
{
    if (preview == NULL || !preview->opened)
        return;                     /* 没打开就不用处理。 */

    disp_layer_display_close(&preview->disp); /* 先隐藏视频层，避免继续扫描已释放 dmabuf。 */
    g2d_display_close(&preview->g2d); /* 再释放 G2D 目标 dmabuf。 */
    image_convert_close(&preview->decoder); /* 最后关闭 CedarC。 */
    memset(preview, 0, sizeof(*preview)); /* 清空状态。 */
    preview->g2d.fd = -1;           /* 恢复无效 fd。 */
}

/* 打开播放列表第一帧预览图层。 */
static int still_preview_open(struct still_preview *preview,
                              const struct t113_ui_rect *rect,
                              unsigned int src_width,
                              unsigned int src_height,
                              enum cedar_video_codec codec)
{
    if (preview == NULL || rect == NULL)
        return -1;                  /* 参数无效。 */
    if (preview->opened)
        return 0;                   /* 已打开则复用。 */

    memset(preview, 0, sizeof(*preview)); /* 清空上下文。 */
    preview->g2d.fd = -1;           /* 标记 G2D 未打开。 */

    if (disp_layer_display_open_ex(&preview->disp, 800, 480,
                                   1, 1, 12,
                                   rect->x, rect->y,
                                   (unsigned int)rect->w,
                                   (unsigned int)rect->h) < 0)
        return -1;                  /* 打开视频层失败。 */

    if (src_width == 0)
        src_width = DEFAULT_WIDTH;  /* 没有容器宽度时回退到摄像头默认宽度。 */
    if (src_height == 0)
        src_height = DEFAULT_HEIGHT; /* 没有容器高度时回退到摄像头默认高度。 */

    if (g2d_display_open_sized(&preview->g2d, src_width, src_height,
                               (unsigned int)rect->w,
                               (unsigned int)rect->h) < 0)
        goto out_disp;              /* 打开 G2D 失败。 */

    if (image_convert_open_codec(&preview->decoder, src_width, src_height,
                                 codec) < 0)
        goto out_g2d;               /* 打开 CedarC 失败。 */

    preview->opened = 1;            /* 标记预览图层已就绪。 */
    return 0;                       /* 打开成功。 */

out_g2d:
    g2d_display_close(&preview->g2d);
out_disp:
    disp_layer_display_close(&preview->disp);
    memset(preview, 0, sizeof(*preview));
    preview->g2d.fd = -1;
    return -1;
}

/* 把 FFmpeg 识别到的压缩格式映射成 CedarC 初始化格式。 */
static enum cedar_video_codec cedar_codec_from_demuxer(enum ffmpeg_video_codec codec)
{
    if (codec == FFMPEG_VIDEO_CODEC_H265)
        return CEDAR_VIDEO_CODEC_H265;
    if (codec == FFMPEG_VIDEO_CODEC_H264)
        return CEDAR_VIDEO_CODEC_H264;
    return CEDAR_VIDEO_CODEC_MJPEG;
}

/* 在播放列表中高亮 MJPEG 文件时，只解码第一帧作为预览图。 */
static int preview_mjpeg_first_frame(struct still_preview *preview,
                                     const char *path)
{
    struct t113_ui_rect video_rect;  /* vedioplay 内容区域。 */
    unsigned char *file_data = NULL; /* MJPEG 文件内容。 */
    const unsigned char *frame_data; /* 第一帧 JPEG 数据。 */
    size_t file_size = 0;            /* 文件长度。 */
    size_t offset = 0;               /* MJPEG 解析偏移。 */
    size_t frame_size;               /* 第一帧 JPEG 长度。 */
    struct yuv_image yuv;            /* CedarC 解码输出。 */
    int ret = -1;                    /* 默认失败。 */
    unsigned int width, height;

    memset(&yuv, 0, sizeof(yuv));    /* 清空 YUV。 */

    if (!t113_ui_lvgl_get_video_rect(&video_rect)) /* 获取 UI 视频框。 */
        return -1;                  /* 获取失败。 */
    if (read_file_all(path, &file_data, &file_size) < 0) /* 读入 MJPEG 文件。 */
        return -1;                  /* 读取失败。 */
    if (!find_next_jpeg_frame(file_data, file_size, &offset,
                              &frame_data, &frame_size))
        goto out_file;              /* 没有完整 JPEG 帧。 */

    if (jpeg_dimensions(frame_data, frame_size, &width, &height) < 0) /* 读取当前 JPEG 的实际 SOF 尺寸，不继承上一张照片的几何。 */
        goto out_file;
    /* Independent photos may change dimensions and tables; never reuse old MJPEG state. */
    still_preview_close(preview); /* 先隐藏旧图层并回收旧尺寸缓冲，再创建新照片的硬件上下文。 */
    if (still_preview_open(preview, &video_rect, width, height,
                           CEDAR_VIDEO_CODEC_MJPEG) < 0)
        goto out_file;

    if (image_convert_decode_mjpeg(&preview->decoder, frame_data, frame_size, &yuv) < 0)
        goto out_file;              /* 解码第一帧失败。 */
    if (g2d_display_stretch_yuv_to_dmabuf(&preview->g2d, &yuv) < 0)
        goto out_yuv;               /* G2D 转换失败。 */
    printf("jpeg preview: %s input=%ux%u visible=%ux%u plane=%ux%u\n",
           path, width, height, yuv.width, yuv.height, yuv.stride, yuv.buffer_height);
    if (disp_layer_display_show_argb8888(&preview->disp,
                                         g2d_display_get_dst_fd(&preview->g2d),
                                         g2d_display_get_dst_width(&preview->g2d),
                                         g2d_display_get_dst_height(&preview->g2d)) < 0)
        goto out_yuv;               /* 提交预览图失败。 */

    ret = 0;                        /* 第一帧预览成功。 */

out_yuv:
    image_convert_free_yuv(&yuv);   /* 释放 CedarC 输出。 */
out_file:
    free(file_data);                /* 释放文件缓冲。 */
    if (ret < 0)                    /* 第一帧预览失败时不能留下半开的资源。 */
        still_preview_close(preview); /* 释放 disp/G2D/CedarC，避免后续录制 OOM。 */
    return ret;                     /* 返回预览结果。 */
}

/* 刷新播放模式选中项；MJPEG 会显示第一帧，不支持格式显示 NO SUPPORT。 */
static void update_playback_selection_preview(struct disp_layer_display *ui_disp,
                                              struct t113_ui_lvgl *ui,
                                              struct still_preview *preview,
                                              const struct media_list *list,
                                              size_t selected,
                                              uint32_t *last_flush)
{
    const struct media_item *item;  /* 当前媒体条目。 */

    show_selected_media_hint(list, selected); /* 更新标题和提示文字。 */
    if (list == NULL || list->count == 0) {
        still_preview_close(preview); /* 空列表时隐藏预览图层。 */
        submit_ui_force(ui_disp, ui, last_flush); /* 空列表只刷新 UI。 */
        return;
    }

    if (storage_session_lost()) return; /* 拔盘后不再访问旧列表路径。 */
    item = &list->items[selected];  /* 读取当前选中项。 */
    submit_ui_force(ui_disp, ui, last_flush); /* 先把标题/提示文字刷出来。 */
    if (item->type == MEDIA_TYPE_MJPEG ||
        item->type == MEDIA_TYPE_JPEG) { /* MJPEG/JPEG 直接找第一帧。 */
        if (preview_mjpeg_first_frame(preview, item->path) < 0) {
            still_preview_close(preview); /* 预览失败时清掉可能残留的视频层。 */
            t113_ui_lvgl_set_video_message("PREVIEW ERROR");
            submit_ui_force(ui_disp, ui, last_flush);
        }
    } else if (item->type == MEDIA_TYPE_MKV ||
               item->type == MEDIA_TYPE_AVI ||
               item->type == MEDIA_TYPE_MP4) {
        /*
         * 容器视频预览会在列表切换时频繁打开 CedarC/G2D/disp。
         * 录制刚结束后的 AVI 文件尤其容易触发显示驱动卡死，所以第一版先禁用自动预览。
         * 真正按下播放键时仍然会进入完整播放链路。
         */
        still_preview_close(preview); /* 先隐藏旧图层并回收旧尺寸缓冲，再创建新照片的硬件上下文。 */
        t113_ui_lvgl_set_video_message("VIDEO");
        submit_ui_force(ui_disp, ui, last_flush);
    } else if (item->type == MEDIA_TYPE_WAV ||
               item->type == MEDIA_TYPE_AAC ||
               item->type == MEDIA_TYPE_MP3) {
        still_preview_close(preview); /* 音频文件没有视频预览。 */
        t113_ui_lvgl_set_video_message("AUDIO");
        submit_ui_force(ui_disp, ui, last_flush);
    } else {
        still_preview_close(preview); /* 不支持文件不保留上一帧预览。 */
    }
}

/* 打开录制模式实时预览，把摄像头画面映射到 UI 的 vedioplay 框。 */
static int record_preview_open(struct record_preview *preview,
                               const struct t113_ui_rect *rect)
{
    if (preview == NULL || rect == NULL)
        return -1;                  /* 参数无效。 */

    memset(preview, 0, sizeof(*preview)); /* 清空上下文。 */
    preview->cap.fd = -1;           /* 标记摄像头未打开。 */
    preview->g2d.fd = -1;           /* 标记 G2D 未打开。 */

    if (disp_layer_display_open_ex(&preview->disp, 800, 480,
                                   1, 1, 12,
                                   rect->x, rect->y,
                                   (unsigned int)rect->w,
                                   (unsigned int)rect->h) < 0)
        return -1;                  /* 打开视频图层失败。 */

    if (g2d_display_open_sized(&preview->g2d, DEFAULT_WIDTH, DEFAULT_HEIGHT,
                               (unsigned int)rect->w,
                               (unsigned int)rect->h) < 0)
        goto out_disp;              /* 打开 G2D 失败。 */

    if (image_convert_open(&preview->decoder, DEFAULT_WIDTH, DEFAULT_HEIGHT) < 0)
        goto out_g2d;               /* 打开 CedarC 失败。 */

    if (video_capture_open(&preview->cap, DEFAULT_DEVICE, DEFAULT_WIDTH, DEFAULT_HEIGHT,
                           V4L2_PIX_FMT_MJPEG) < 0)
        goto out_decoder;           /* 打开摄像头失败。 */

    if (video_capture_start(&preview->cap) < 0)
        goto out_cap;               /* 启动摄像头失败。 */

    preview->opened = 1;            /* 标记资源已就绪。 */
    return 0;                       /* 打开成功。 */

out_cap:
    video_capture_close(&preview->cap);
out_decoder:
    image_convert_close(&preview->decoder);
out_g2d:
    g2d_display_close(&preview->g2d);
out_disp:
    disp_layer_display_close(&preview->disp);
    memset(preview, 0, sizeof(*preview));
    preview->cap.fd = -1;
    preview->g2d.fd = -1;
    return -1;
}

/* 录制模式预览一帧；保存与显示消费同一采集帧。 */
static int record_preview_step(struct record_preview *preview)
{
    struct video_frame frame;       /* 摄像头 MJPG 帧。 */
    struct yuv_image yuv;           /* CedarC 解码输出。 */
    int ret;                        /* 临时返回值。 */

    if (preview == NULL || !preview->opened)
        return 0;                   /* 未打开时不用处理。 */

    memset(&yuv, 0, sizeof(yuv));   /* 清空 YUV 输出。 */
    if (video_capture_get_frame(&preview->cap, &frame) < 0)
        return -1;                  /* 取帧失败。 */

    if (preview->recording) {       /* 如果用户已经按下开始录制。 */
        if (audio_queue_is_stopped(&preview->audio_queue)) { /* 采集线程故障或会话取消时不能继续写只有视频的成功录像。 */
            record_writer_abort(&preview->sink); /* 发布粘滞失败，让唯一写线程中止并禁止正式文件发布。 */
            video_capture_put_frame(&preview->cap, &frame); /* 保存或复制消费完后归还驱动槽；之后禁止读取 frame.data。 */
            return -1;
        }
        if (record_writer_submit(&preview->sink, frame.data, frame.size, /* 把压缩 JPEG 副本和采集时间送入有界写队列，不在预览线程写盘。 */
                                  frame.timestamp_us) < 0) { /* 队列提交失败也必须归还 V4L2 帧，并向上层报告失败。 */
            video_capture_put_frame(&preview->cap, &frame); /* 写失败也要归还 V4L2 buffer。 */
            return -1;              /* 通知上层录制/预览异常。 */
        }
    }

    ret = image_convert_decode_mjpeg(&preview->decoder, /* 预览分支硬解 JPEG；保存分支直接封装原始压缩帧。 */
                                     frame.data, frame.size, &yuv);
    video_capture_put_frame(&preview->cap, &frame); /* 取出后尽快归还 V4L2 buffer。 */
    if (ret < 0)
        return -1;                  /* 解码失败。 */

    ret = g2d_display_stretch_yuv_to_dmabuf(&preview->g2d, &yuv); /* G2D 转换并缩放到独立输出页，完成后可以归还 CedarC 图像。 */
    image_convert_free_yuv(&yuv);   /* 释放 CedarC 输出帧。 */
    if (ret < 0)
        return -1;                  /* G2D 失败。 */

    ret = disp_layer_display_show_argb8888(&preview->disp,
                                           g2d_display_get_dst_fd(&preview->g2d),
                                           g2d_display_get_dst_width(&preview->g2d),
                                           g2d_display_get_dst_height(&preview->g2d));
    if (ret < 0)
        return -1;                  /* 显示失败。 */

    preview->frames++;              /* 统计预览帧。 */
    return 0;                       /* 一帧显示成功。 */
}

/* 开始 UI 录制模式：把预览中的 MJPG 帧和 ALSA PCM 同步写入 AVI。 */
static int record_preview_start_record(struct record_preview *preview,
                                       const char *record_name)
{
    char unique_name[128]; /* 选中 record 名称被占用时生成后缀，不覆盖已有录像。 */
    if (preview == NULL || !preview->opened)
        return -1;                  /* 必须先打开摄像头预览。 */
    if (preview->recording)
        return 0;                   /* 已经在录制就不用重复打开。 */

    if (storage_session_lost() || !media_storage_ready(USB_ROOT, 1))
        return -1;                  /* 新任务不得访问失效或只读的存储会话。 */

    if (media_storage_next_name(USB_ROOT, "VIDEO", record_name, "avi",
                                unique_name, sizeof(unique_name)) < 0)
        return -1;
    if (ffmpeg_mjpeg_muxer_open_av_named(&preview->writer, USB_ROOT, unique_name,
                                         DEFAULT_WIDTH, DEFAULT_HEIGHT, PLAYER_FPS,
                                         AUDIO_CAPTURE_RATE,
                                         AUDIO_CAPTURE_CHANNELS,
                                         AUDIO_CAPTURE_BITS_PER_SAMPLE,
                                         preview->record_path,
                                         sizeof(preview->record_path)) < 0)
        return -1;                  /* U 盘不可用或文件打不开。 */

    if (audio_queue_init(&preview->audio_queue, AUDIO_QUEUE_CAPACITY) < 0) /* 创建有界 PCM 队列，采集和文件写入通过它解耦。 */
        goto fail_writer;           /* 音频队列初始化失败。 */
    preview->audio_queue_ready = 1; /* 标记音频队列需要销毁。 */
    if (record_writer_start_cancelable(&preview->sink, &preview->writer, &preview->audio_queue, /* 启动唯一封装写线程，主线程仅提交 JPEG 和停止请求。 */
                                      g_storage_enabled ? &g_storage.cancel : NULL) < 0) /* 录制会话绑定存储粘滞取消标志，快速重插不能续写旧文件。 */
        goto fail_audio_queue;

    memset(&preview->audio_worker, 0, sizeof(preview->audio_worker)); /* 初始化采集统计与结果；此上下文在线程 join 前保持存活。 */
    preview->audio_worker.queue = &preview->audio_queue; /* PCM 输出到录制写线程消费的队列。 */
    preview->audio_worker.device = AUDIO_CAPTURE_DEVICE; /* 使用板载 codec 采集设备，不是 USB 摄像头麦克风。 */
    preview->audio_worker.rate = AUDIO_CAPTURE_RATE; /* 采用实测稳定的 48kHz，与 AVI 音频流声明一致。 */
    preview->audio_worker.channels = AUDIO_CAPTURE_CHANNELS; /* 实测 mono/ADC1 路径，不沿用早期三声道采集。 */
    preview->audio_worker.period_frames = AUDIO_CAPTURE_PERIOD_FRAMES; /* 一次读取的目标样本帧数，影响采集提交粒度。 */
    preview->audio_worker.ret = -1; /* 默认失败，只有采集线程正常退出才设置成功。 */
    if (pthread_create(&preview->audio_thread, NULL,
                       audio_capture_thread_main,
                       &preview->audio_worker) != 0) {
        perror("pthread_create audio capture");
        goto fail_audio_queue;
    }
    preview->audio_started = 1;     /* 音频从录制开始时同步采集。 */
    preview->recording = 1;         /* 从下一帧开始写入文件。 */
    return 0;                       /* 开始成功。 */

fail_audio_queue:
    record_writer_abort(&preview->sink); /* 发布粘滞失败，让唯一写线程中止并禁止正式文件发布。 */
    record_writer_finish(&preview->sink); /* 等待写线程退出；此后才能关闭封装器和释放 PCM 队列。 */
    audio_queue_destroy(&preview->audio_queue); /* 采集和写线程已停止后回收残余音频块。 */
    preview->audio_queue_ready = 0;
fail_writer:
    ffmpeg_mjpeg_muxer_abort(&preview->writer); /* 启动未完成，只保留临时文件。 */
    ffmpeg_mjpeg_muxer_close(&preview->writer); /* 启动失败时清理封装资源，保留失败 .part 不发布 AVI。 */
    return -1;
}

/* 摄像头已输出 JPEG 压缩帧，拍照直接保存，不需要再次编码。 */
static int record_preview_photo(struct record_preview *preview, const char *name)
{
    struct video_frame frame;
    char path[512];
    int ret;
    if (!preview->opened || preview->recording ||
        storage_session_lost() || !media_storage_ready(USB_ROOT, 1) ||
        video_capture_get_frame(&preview->cap, &frame) < 0)
        return -1;
    ret = media_storage_save_jpeg_cancelable(USB_ROOT, name, frame.data, frame.size, path, sizeof(path), /* 拍照复用摄像头原始 JPEG，保存过程中检查存储取消。 */
                                            g_storage_enabled ? &g_storage.cancel : NULL); /* 取消时不得报告保存成功。 */
    video_capture_put_frame(&preview->cap, &frame); /* 保存或复制消费完后归还驱动槽；之后禁止读取 frame.data。 */
    if (ret == 0)
        printf("photo saved : %s bytes=%zu\n", path, frame.size);
    else
        fprintf(stderr, "photo save failed: %s\n", strerror(errno));
    return ret;
}

/* 停止 UI 录制模式，并把文件同步到 U 盘。 */
static int record_preview_stop_record(struct record_preview *preview)
{
    int ret = 0;                    /* 关闭结果。 */

    if (preview == NULL || !preview->recording)
        return 0;                   /* 没在录制，无需处理。 */

    preview->recording = 0;         /* 先关标志，避免后续帧继续写。 */
    if (storage_session_lost()) {   /* 拔出后不能走正常保存路径，即使旧文件仍能缓存写入。 */
        record_writer_abort(&preview->sink); ret = -1; /* 不发布损坏的正式 AVI。 */
    }
    if (preview->audio_started) {
        audio_queue_stop(&preview->audio_queue); /* 通知音频采集线程退出。 */
        pthread_join(preview->audio_thread, NULL); /* 等待 ALSA 采集线程收尾。 */
        if (preview->audio_worker.ret < 0)
            ret = -1;               /* ALSA 采集失败也必须反馈到保存结果。 */
        preview->audio_started = 0; /* 标记音频线程已结束。 */
    }
    if (ret < 0)
        record_writer_abort(&preview->sink); /* 发布粘滞失败，让唯一写线程中止并禁止正式文件发布。 */
    if (record_writer_finish(&preview->sink) < 0) /* 正常停止要求队列排空；写线程错误必须传播给保存结果。 */
        ret = -1;
    if (ret < 0)
        ffmpeg_mjpeg_muxer_abort(&preview->writer); /* 禁止正常写尾和发布，避免残缺录像混入可播放列表。 */
    printf("record timing: video=%lldms audio=%llums capture_drift=%lldus\n",
           (long long)preview->writer.last_pts,
           (unsigned long long)(preview->writer.audio_frames * 1000 / AUDIO_CAPTURE_RATE),
           (long long)(preview->audio_worker.last_sample_us - preview->audio_worker.first_sample_us -
                        (int64_t)preview->audio_worker.captured_frames * 1000000 / AUDIO_CAPTURE_RATE));
    if (ffmpeg_mjpeg_muxer_close(&preview->writer) < 0) /* 写尾、同步并发布 AVI。 */
        ret = -1;
    if (preview->audio_queue_ready) {
        audio_queue_destroy(&preview->audio_queue); /* 销毁音频队列。 */
        preview->audio_queue_ready = 0;
    }
    return ret;                     /* 返回关闭结果。 */
}

/* 关闭录制模式预览，并隐藏视频图层。 */
static void record_preview_close(struct record_preview *preview)
{
    if (preview == NULL || !preview->opened)
        return;                     /* 没打开无需处理。 */

    record_preview_stop_record(preview); /* 如果正在录制，退出预览前先收尾文件。 */
    video_capture_stop(&preview->cap); /* 停摄像头 stream。 */
    video_capture_close(&preview->cap); /* 关闭摄像头。 */
    disp_layer_display_close(&preview->disp); /* 先隐藏视频层，避免继续扫描已释放 dmabuf。 */
    hide_video_overlay_layer();     /* 再保险地隐藏一次 overlay。 */
    g2d_display_close(&preview->g2d); /* 再释放 G2D 目标 dmabuf。 */
    image_convert_close(&preview->decoder); /* 最后关闭 CedarC。 */
    memset(preview, 0, sizeof(*preview)); /* 清空上下文。 */
    preview->cap.fd = -1;           /* 恢复无效 fd。 */
    preview->g2d.fd = -1;           /* 恢复无效 fd。 */
}

/* MJPEG 裸流读取线程：扫描文件内存，把每帧复制到播放队列。 */
static void *mjpeg_source_thread(void *arg)
{
    struct mjpeg_source_worker *worker = arg; /* 线程上下文。 */
    size_t offset = 0;              /* 当前扫描偏移。 */
    unsigned int index = 0;         /* 帧序号。 */

    worker->ret = 0;                /* 默认成功。 */
    while (!g_stop) {               /* 没有 Ctrl+C 时持续读取。 */
        const unsigned char *frame; /* 当前 JPEG 帧。 */
        size_t frame_size;          /* 当前帧大小。 */
        struct playback_packet *packet; /* 队列 packet。 */

        if (!find_next_jpeg_frame(worker->data, worker->size, &offset,
                                  &frame, &frame_size))
            break;                  /* 没有更多帧。 */
        packet = playback_packet_create(frame, frame_size, -1, -1, index); /* 裸 JPEG 无 PTS，创建自有数据副本供播放队列跨线程消费。 */
        if (packet == NULL) {       /* 内存不足。 */
            worker->ret = -1;       /* 记录失败。 */
            break;                  /* 退出线程。 */
        }
        if (playback_queue_push(worker->queue, packet) < 0) { /* 入队失败。 */
            playback_packet_free(packet); /* 释放未入队 packet。 */
            break;                  /* 队列停止。 */
        }
        index++;                    /* 下一帧序号。 */
    }
    playback_queue_finish(worker->queue); /* 通知消费者没有新帧了。 */
    return NULL;                    /* 线程结束。 */
}

/* 容器读取线程：FFmpeg 解封装 MJPEG/H264/H265，复制压缩包到播放队列；不软件解码。 */
static void *mkv_source_thread(void *arg)
{
    struct mkv_source_worker *worker = arg; /* 线程上下文。 */
    unsigned int index = 0;         /* 帧序号。 */

    worker->ret = 0;                /* 默认成功。 */
    while (!g_stop) {               /* 没有 Ctrl+C 时持续读取。 */
        const unsigned char *frame; /* 当前 MJPEG packet。 */
        size_t frame_size;          /* packet 大小。 */
        int64_t pts_ms;             /* packet PTS。 */
        int64_t duration_ms;        /* packet duration。 */
        int read_ret;               /* demux 读取结果。 */
        struct playback_packet *packet; /* 队列 packet。 */

        read_ret = ffmpeg_mjpeg_demuxer_read(worker->demuxer, &frame, &frame_size,
                                             &pts_ms, &duration_ms);
        if (read_ret == 0)          /* 正常文件结束。 */
            break;                  /* 退出线程。 */
        if (read_ret < 0) {         /* 读取失败。 */
            worker->ret = -1;       /* 记录错误。 */
            break;                  /* 退出线程。 */
        }
        packet = playback_packet_create(frame, frame_size, pts_ms, /* 复制解封装借用数据并保留 PTS，下一次 read 不会覆盖此 packet。 */
                                        duration_ms, index);
        if (packet == NULL) {       /* 内存不足。 */
            worker->ret = -1;       /* 记录失败。 */
            break;                  /* 退出线程。 */
        }
        if (playback_queue_push(worker->queue, packet) < 0) { /* 入队失败。 */
            playback_packet_free(packet); /* 释放未入队 packet。 */
            break;                  /* 队列停止。 */
        }
        index++;                    /* 下一帧。 */
    }
    playback_queue_finish(worker->queue); /* 通知消费者没有新帧了。 */
    return NULL;                    /* 线程结束。 */
}

/* 容器音频线程：独立解封装并解码音轨，转换 PCM 送 ALSA，再发布硬件消费时钟。 */
static void *avi_audio_thread(void *arg)
{
    struct avi_audio_worker *worker = arg; /* 线程上下文。 */

    worker->ret = ffmpeg_audio_play_clock(worker->path,
                                    AUDIO_CAPTURE_DEVICE,
                                    worker->stop,
                                    worker->paused,
                                    NULL,
                                    NULL, worker->clock);
    __atomic_store_n(&worker->done, 1, __ATOMIC_RELEASE); /* 先写结果再发布 done，读取线程 acquire 后才可以访问 ret。 */
    return NULL;
}


/* FFmpeg 音频播放线程：播放 AAC/MP3 等压缩音频。 */
static void *ffmpeg_audio_play_thread(void *arg)
{
    struct wav_play_worker *worker = arg; /* 复用同样的播放线程上下文。 */

    worker->ret = ffmpeg_audio_play(worker->path,
                                    AUDIO_CAPTURE_DEVICE,
                                    &worker->stop,
                                    &worker->paused,
                                    wav_progress_cb,
                                    worker);
    __atomic_store_n(&worker->done, 1, __ATOMIC_RELEASE); /* 先写结果再发布 done，读取线程 acquire 后才可以访问 ret。 */
    return NULL;
}

/* 在已经打开的 LVGL UI 上播放 AAC/MP3 等 FFmpeg 可解码音频。 */
static enum playback_result play_ffmpeg_audio_on_current_ui(struct disp_layer_display *ui_disp,
                                                            struct t113_ui_lvgl *ui,
                                                            struct input_key *input,
                                                            const char *path,
                                                            uint32_t *last_flush)
{
    struct ffmpeg_audio_info info;  /* 音频文件信息。 */
    struct wav_play_worker worker;  /* 音频播放线程参数。 */
    pthread_t thread;               /* 音频播放线程。 */
    int thread_started = 0;         /* 线程是否已启动。 */
    enum playback_result ret = PLAYBACK_RESULT_ERROR; /* 默认失败。 */

    memset(&worker, 0, sizeof(worker)); /* 绑定前初始化停止标志，不得覆盖监控线程发出的取消。 */
    storage_bind_task(&worker.stop); /* 即使阻塞在探测阶段也能收到拔盘通知。 */
    if (ffmpeg_audio_probe_interruptible(path, &info, &worker.stop) < 0) {
        storage_bind_task(NULL);    /* 所有返回路径必须解绑栈上的停止标志。 */
        t113_ui_lvgl_set_audio_visualizer(false);
        t113_ui_lvgl_set_video_message("NO SUPPORT");
        submit_ui_force(ui_disp, ui, last_flush);
        return PLAYBACK_RESULT_ERROR;
    }

    worker.path = path;
    worker.total_ms = info.duration_ms > 0 ? (uint32_t)info.duration_ms : 1U;
    t113_ui_lvgl_set_video_message("");
    t113_ui_lvgl_set_audio_visualizer(true);
    t113_ui_lvgl_set_fps(0);
    set_playback_progress_ms(0, worker.total_ms);
    submit_ui_force(ui_disp, ui, last_flush);

    if (pthread_create(&thread, NULL, ffmpeg_audio_play_thread, &worker) != 0) {
        perror("pthread_create ffmpeg audio");
        storage_bind_task(NULL);    /* 创建失败也不能留下悬空绑定。 */
        return PLAYBACK_RESULT_ERROR;
    }
    thread_started = 1;

    while (!g_stop && !storage_session_lost() && !__atomic_load_n(&worker.done, __ATOMIC_ACQUIRE)) {
        if (input != NULL && input->fd >= 0) {
            struct app_key_event event;

            if (input_key_poll(input, &event) > 0) {
                if (event.key == APP_KEY_BACK &&
                    event.action == APP_KEY_ACTION_SHORT) {
                    __atomic_store_n(&worker.paused, !__atomic_load_n(&worker.paused, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
                    t113_ui_lvgl_set_playing(!worker.paused);
                    submit_ui_force(ui_disp, ui, last_flush);
                } else if (event.key == APP_KEY_NEXT &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    ret = PLAYBACK_RESULT_NEXT;
                    __atomic_store_n(&worker.stop, 1, __ATOMIC_RELEASE); /* 请求音频线程退出，不从控制线程关闭它持有的 ALSA 句柄。 */
                    break;
                } else if (event.key == APP_KEY_ENTER &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    ret = PLAYBACK_RESULT_PREV;
                    __atomic_store_n(&worker.stop, 1, __ATOMIC_RELEASE); /* 请求音频线程退出，不从控制线程关闭它持有的 ALSA 句柄。 */
                    break;
                } else if (event.key == APP_KEY_BACK && event.action == APP_KEY_ACTION_LONG) {
                    ret = PLAYBACK_RESULT_RECORD;
                    break;
                }
            }
        }

        set_playback_progress_ms(__atomic_load_n(&worker.played_ms, __ATOMIC_ACQUIRE),
                                 __atomic_load_n(&worker.total_ms, __ATOMIC_ACQUIRE));
        submit_ui_if_dirty(ui_disp, ui, last_flush);
        usleep(10000);
    }

    __atomic_store_n(&worker.stop, 1, __ATOMIC_RELEASE); /* 请求音频线程退出，不从控制线程关闭它持有的 ALSA 句柄。 */
    if (thread_started)
        pthread_join(thread, NULL); /* 等待音频任务完全退出，再释放栈上的 worker 和解除会话绑定。 */
    storage_bind_task(NULL);        /* join 后解绑，监控不再访问 worker。 */
    if (storage_session_lost()) ret = PLAYBACK_RESULT_STORAGE_LOST; /* 与正常 EOF 明确区分。 */
    /* join 后才能读取线程返回值，包括 Ctrl+C 提前退出的路径。 */
    if (ret == PLAYBACK_RESULT_ERROR)
        ret = worker.ret == 0 ? PLAYBACK_RESULT_END : PLAYBACK_RESULT_ERROR;
    t113_ui_lvgl_set_audio_visualizer(false);
    t113_ui_lvgl_set_video_message("");
    submit_ui_force(ui_disp, ui, last_flush);
    return ret;
}

/* 在已经打开的 LVGL UI 上播放 WAV。 */

/* 在已经打开的 LVGL UI 上播放一个 MJPEG 裸流文件。 */
static enum playback_result play_mjpeg_on_current_ui(struct disp_layer_display *ui_disp,
                                                     struct t113_ui_lvgl *ui,
                                                     struct input_key *input,
                                                     const char *path,
                                                     uint32_t *last_flush)
{
    struct disp_layer_display video_disp; /* 视频叠加图层。 */
    struct t113_ui_rect video_rect;  /* vedioplay 内容区域。 */
    struct cedar_decoder decoder;    /* CedarC MJPG 解码器。 */
    struct g2d_display g2d;          /* G2D 缩放和格式转换。 */
    struct playback_packet_queue queue; /* 读取线程到显示线程的队列。 */
    struct mjpeg_source_worker source; /* MJPEG 读取线程上下文。 */
    pthread_t source_thread;        /* MJPEG 读取线程。 */
    struct timespec schedule_start; /* 绝对时间调度起点。 */
    unsigned char *file_data = NULL; /* MJPEG 文件内容。 */
    size_t file_size = 0;            /* 文件长度。 */
    unsigned int frames = 0;         /* 已播放帧数。 */
    unsigned int total_frames = 0;   /* 文件总帧数，用于进度条。 */
    struct fps_meter fps;            /* 实际播放帧率统计。 */
    int paused = 0;                  /* 播放暂停状态。 */
    int source_started = 0;          /* 读取线程是否已启动。 */
    enum playback_result ret = PLAYBACK_RESULT_ERROR; /* 默认失败。 */

    memset(&video_disp, 0, sizeof(video_disp)); /* 清空视频图层。 */
    memset(&decoder, 0, sizeof(decoder)); /* 清空解码器。 */
    memset(&g2d, 0, sizeof(g2d));    /* 清空 G2D。 */
    memset(&source, 0, sizeof(source)); /* 清空读取线程上下文。 */
    g2d.fd = -1;                    /* 标记 G2D 未打开。 */
    if (playback_queue_init(&queue) < 0) { /* 初始化播放队列。 */
        fprintf(stderr, "playback queue init failed\n");
        return PLAYBACK_RESULT_ERROR;
    }

    if (!t113_ui_lvgl_get_video_rect(&video_rect)) { /* 获取显示区域。 */
        fprintf(stderr, "get UI video rect failed\n");
        goto out_file;
    }
    if (read_file_all(path, &file_data, &file_size) < 0) /* 读入 MJPEG 文件。 */
        goto out_file;
    total_frames = count_mjpeg_frames(file_data, file_size); /* 统计总帧数。 */
    if (total_frames == 0) {        /* 没有完整 JPEG 帧。 */
        fprintf(stderr, "no jpeg frame found in %s\n", path);
        goto out_file;              /* 退出播放。 */
    }

    t113_ui_lvgl_set_audio_visualizer(false);
    t113_ui_lvgl_set_video_message(""); /* 播放前清掉 NO SUPPORT。 */
    t113_ui_lvgl_set_fps(0);        /* 播放开始前清空 FPS。 */
    set_playback_progress(0, total_frames); /* 播放进度从 0ms 开始。 */
    submit_ui_if_dirty(ui_disp, ui, last_flush); /* 立即刷新 UI。 */
    fps_meter_reset(&fps);          /* 开始统计实际播放帧率。 */
    memset(&schedule_start, 0, sizeof(schedule_start)); /* 第一帧真正显示前再启动调度时钟。 */

    if (disp_layer_display_open_ex(&video_disp, 800, 480,
                                   1, 1, 12,
                                   video_rect.x, video_rect.y,
                                   (unsigned int)video_rect.w,
                                   (unsigned int)video_rect.h) < 0)
        goto out_file;              /* 打开视频层失败。 */

    if (g2d_display_open_sized(&g2d, DEFAULT_WIDTH, DEFAULT_HEIGHT,
                               (unsigned int)video_rect.w,
                               (unsigned int)video_rect.h) < 0)
        goto out_video_disp;        /* 打开 G2D 失败。 */

    if (image_convert_open(&decoder, DEFAULT_WIDTH, DEFAULT_HEIGHT) < 0)
        goto out_g2d;               /* 打开 CedarC 失败。 */

    source.queue = &queue;          /* 设置读取线程输出队列。 */
    source.data = file_data;        /* 设置文件内存。 */
    source.size = file_size;        /* 设置文件大小。 */
    if (pthread_create(&source_thread, NULL, mjpeg_source_thread, &source) != 0) {
        perror("pthread_create mjpeg source");
        goto out_decoder;
    }
    source_started = 1;             /* 标记线程已启动。 */

    while (!g_stop && !storage_session_lost()) { /* 拔盘时即使裸流已全部读入内存也停止显示。 */
        struct playback_packet *packet; /* 当前待显示 packet。 */
        struct yuv_image yuv;       /* CedarC 解码输出。 */

        if (input != NULL && input->fd >= 0) { /* 播放中也继续响应实体按键。 */
            struct app_key_event event; /* 播放过程中的按键事件。 */

            if (input_key_poll(input, &event) > 0) {
                if (event.key == APP_KEY_BACK &&
                    event.action == APP_KEY_ACTION_SHORT) {
                    paused = !paused; /* 中键短按：播放/暂停切换。 */
                    if (!paused) {  /* 从暂停恢复播放。 */
                        clock_gettime(CLOCK_MONOTONIC, &schedule_start);
                        schedule_start = timespec_sub_ms(schedule_start,
                                                         playback_due_ms(frames, -1));
                    }
                    t113_ui_lvgl_set_playing(!paused); /* 图标随状态变化。 */
                    submit_ui_force(ui_disp, ui, last_flush); /* 立刻刷新按钮图标。 */
                } else if (event.key == APP_KEY_NEXT &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    ret = PLAYBACK_RESULT_NEXT; /* 右键：停止当前播放，回主循环切下一项。 */
                    goto out_decoder;
                } else if (event.key == APP_KEY_ENTER &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    ret = PLAYBACK_RESULT_PREV; /* 左键：停止当前播放，回主循环切上一项。 */
                    goto out_decoder;
                } else if (event.key == APP_KEY_BACK && event.action == APP_KEY_ACTION_LONG) {
                    ret = PLAYBACK_RESULT_RECORD;
                    goto out_decoder;
                }
            }
        }

        if (paused) {               /* 暂停时不再取下一帧。 */
            submit_ui_if_dirty(ui_disp, ui, last_flush); /* 仍允许 UI 自己刷新。 */
            usleep(10000);          /* 小睡，避免暂停状态空转占满 CPU。 */
            continue;               /* 等下一次按键恢复播放。 */
        }

        if (playback_queue_pop(&queue, &packet) == 0) /* 队列结束。 */
            break;                  /* 播放结束或被停止。 */

        memset(&yuv, 0, sizeof(yuv)); /* 清空输出结构。 */
        if (image_convert_decode_mjpeg(&decoder, packet->data, packet->size, &yuv) < 0) {
            playback_packet_free(packet);
            goto out_decoder;       /* 解码失败。 */
        }

        if (g2d_display_stretch_yuv_to_dmabuf(&g2d, &yuv) < 0) { /* G2D 转换。 */
            image_convert_free_yuv(&yuv);
            playback_packet_free(packet);
            goto out_decoder;
        }
        image_convert_free_yuv(&yuv); /* 释放解码输出。 */

        sleep_until_monotonic(timespec_add_ms(schedule_start,
                              playback_due_ms(packet->index, packet->pts_ms)));
        if (disp_layer_display_show_argb8888(&video_disp,
                                             g2d_display_get_dst_fd(&g2d),
                                             g2d_display_get_dst_width(&g2d),
                                             g2d_display_get_dst_height(&g2d)) < 0)
        {
            playback_packet_free(packet);
            goto out_decoder;       /* 提交视频层失败。 */
        }

        frames++;                   /* 统计帧数。 */
        fps_meter_frame(&fps);      /* 每秒刷新一次 FPS 标签。 */
        set_playback_progress(frames, total_frames); /* 当前播放时长 / 总时长。 */
        submit_ui_if_dirty(ui_disp, ui, last_flush); /* 播放时仍刷新 UI。 */
        playback_packet_free(packet); /* 释放当前 packet。 */
    }

    ret = storage_session_lost() ? PLAYBACK_RESULT_STORAGE_LOST : PLAYBACK_RESULT_END; /* 拔盘不是正常 EOF。 */

out_decoder:
    playback_queue_stop(&queue);    /* 通知读取线程退出。 */
    if (source_started)             /* 如果读取线程已经启动。 */
        pthread_join(source_thread, NULL); /* 等待线程结束。 */
    disp_layer_display_close(&video_disp); /* 先隐藏视频层，避免继续扫描 G2D dmabuf。 */
    g2d_display_close(&g2d);        /* 再关闭 G2D。 */
    image_convert_close(&decoder);  /* 最后关闭 CedarC。 */
    goto out_file;
out_g2d:
    disp_layer_display_close(&video_disp); /* G2D 打开失败后也确保视频层关闭。 */
    g2d_display_close(&g2d);        /* 关闭 G2D。 */
    goto out_file;
out_video_disp:
    disp_layer_display_close(&video_disp); /* 关闭视频层。 */
out_file:
    playback_queue_destroy(&queue); /* 销毁播放队列。 */
    free(file_data);                /* 释放文件缓冲。 */
    printf("ui video frames: %u\n", frames); /* 打印播放帧数。 */
    return ret;                     /* 返回结果。 */
}

/* 在已经打开的 LVGL UI 上播放 MKV，FFmpeg 只负责解封装，CedarC 负责解码。 */
static enum playback_result play_mkv_on_current_ui(struct disp_layer_display *ui_disp,
                                                   struct t113_ui_lvgl *ui,
                                                   struct input_key *input,
                                                   const char *path,
                                                   uint32_t *last_flush)
{
    struct disp_layer_display video_disp; /* 视频叠加图层。 */
    struct t113_ui_rect video_rect;  /* vedioplay 内容区域。 */
    struct ffmpeg_mjpeg_demuxer demuxer; /* MKV 解封装器。 */
    struct cedar_decoder decoder;    /* CedarC MJPG 解码器。 */
    struct g2d_display g2d;          /* G2D 缩放和格式转换。 */
    struct playback_packet_queue queue; /* 解封装线程到显示线程的队列。 */
    struct mkv_source_worker source; /* MKV 解封装线程上下文。 */
    pthread_t source_thread;        /* MKV 解封装线程。 */
    struct avi_audio_worker audio_worker; /* AVI PCM 音频播放线程上下文。 */
    pthread_t audio_thread;         /* AVI PCM 音频播放线程。 */
    struct media_clock audio_clock; /* 音频实际消费位置的主时钟，避免视频按读包速度显示。 */
    struct ffmpeg_audio_info audio_info;
    struct yuv_image pending_yuv = {0}; /* 持有一张已解码待显示图，生命周期跨越多次调度循环。 */
    int have_yuv = 0; /* 记录是否拥有待归还的 CedarC 输出。 */
    int video_eof = 0; /* 解封装 EOF 后还需排出解码器缓存，不能立即宣告播放结束。 */
    int clock_ready = 0; /* 记录时钟锁初始化是否成功，错误出口据此销毁。 */
    int64_t pending_due_ms = 0; /* 待显示图像的媒体 PTS，而非当前线程运行时间。 */
    int64_t pause_start_ms = 0; /* 无音频时用于扣除暂停期间的墙钟经过时间。 */
    unsigned int dropped = 0; /* 统计丢弃的过时输出图像，不是丢压缩参考帧。 */
    int64_t last_report_ms = 0;
    int64_t max_display_error_ms = 0; /* 记录 VSYNC 显示后相对主时钟的最大落后。 */
    int timing_log = getenv("AV_TIMING") != NULL; /* 详细时间诊断按需启用，默认不刷每帧日志。 */
    struct timespec schedule_start; /* 绝对时间调度起点。 */
    unsigned int frames = 0;         /* 已播放帧数。 */
    struct fps_meter fps;            /* 实际播放帧率统计。 */
    int paused = 0;                  /* 播放暂停状态。 */
    int source_started = 0;          /* 解封装线程是否已启动。 */
    int audio_started = 0;           /* 音频线程是否已启动。 */
    volatile int audio_stop = 0;     /* 通知音频线程退出。 */
    volatile int audio_paused = 0;   /* 通知音频线程暂停。 */
    int decode_ret;                  /* CedarC 单包解码结果：1 有图，0 延迟，-1 错误。 */
    enum playback_result ret = PLAYBACK_RESULT_ERROR; /* 默认失败。 */

    memset(&video_disp, 0, sizeof(video_disp)); /* 清空视频图层。 */
    memset(&demuxer, 0, sizeof(demuxer)); /* 清空解封装器。 */
    memset(&decoder, 0, sizeof(decoder)); /* 清空解码器。 */
    memset(&g2d, 0, sizeof(g2d));    /* 清空 G2D。 */
    memset(&source, 0, sizeof(source)); /* 清空线程上下文。 */
    memset(&audio_worker, 0, sizeof(audio_worker)); /* 清空音频线程上下文。 */
    g2d.fd = -1;                    /* 标记 G2D 未打开。 */
    if (playback_queue_init(&queue) < 0) { /* 初始化播放队列。 */
        fprintf(stderr, "playback queue init failed\n");
        return PLAYBACK_RESULT_ERROR;
    }
    if (media_clock_init(&audio_clock) != 0) /* 线程启动前创建时钟同步锁。 */
        goto out_queue;
    clock_ready = 1;
    storage_bind_task(&audio_stop); /* 同一个停止标志取消音频和容器读取任务。 */

    if (!t113_ui_lvgl_get_video_rect(&video_rect)) { /* 获取显示区域。 */
        fprintf(stderr, "get UI video rect failed\n");
        goto out_queue;
    }
    if (ffmpeg_mjpeg_demuxer_open_interruptible(&demuxer, path, &audio_stop) < 0)
        goto out_queue;

    t113_ui_lvgl_set_audio_visualizer(false);
    t113_ui_lvgl_set_video_message(""); /* 播放前清掉 NO SUPPORT。 */
    t113_ui_lvgl_set_fps(0);        /* 播放开始前清空 FPS。 */
    set_playback_progress_ms(0, demuxer.duration_ms); /* 进度从 0ms 开始。 */
    submit_ui_if_dirty(ui_disp, ui, last_flush); /* 立即刷新 UI。 */
    fps_meter_reset(&fps);          /* 开始统计实际播放帧率。 */
    clock_gettime(CLOCK_MONOTONIC, &schedule_start); /* 绝对时间调度起点。 */

    if (disp_layer_display_open_ex(&video_disp, 800, 480,
                                   1, 1, 12,
                                   video_rect.x, video_rect.y,
                                   (unsigned int)video_rect.w,
                                   (unsigned int)video_rect.h) < 0)
        goto out_demux;             /* 打开视频层失败。 */

    if (g2d_display_open_sized(&g2d,
                               demuxer.width > 0 ? demuxer.width : DEFAULT_WIDTH,
                               demuxer.height > 0 ? demuxer.height : DEFAULT_HEIGHT,
                               (unsigned int)video_rect.w,
                               (unsigned int)video_rect.h) < 0)
        goto out_video_disp;        /* 打开 G2D 失败。 */

    if (image_convert_open_codec(&decoder,
                                 demuxer.width > 0 ? demuxer.width : DEFAULT_WIDTH,
                                 demuxer.height > 0 ? demuxer.height : DEFAULT_HEIGHT,
                                 cedar_codec_from_demuxer(demuxer.codec)) < 0)
        goto out_g2d;               /* 打开 CedarC 失败。 */

    source.queue = &queue;          /* 设置输出队列。 */
    source.demuxer = &demuxer;      /* 设置 FFmpeg 解封装器。 */
    if (pthread_create(&source_thread, NULL, mkv_source_thread, &source) != 0) {
        perror("pthread_create mkv source");
        goto out_decoder;
    }
    source_started = 1;             /* 标记线程已启动。 */

    audio_worker.path = path;       /* 同一个容器文件单独打开一遍取音频。 */
    audio_worker.stop = &audio_stop; /* 音频播放和 FFmpeg 视频读取共用会话停止信号。 */
    audio_worker.paused = &audio_paused; /* 音频线程按同一暂停状态停止提交声音。 */
    audio_worker.clock = &audio_clock; /* 仅发布时钟，不把 ALSA 句柄交给视频线程。 */
    if (ffmpeg_audio_probe_interruptible(path, &audio_info, &audio_stop) == 0) {
        if (pthread_create(&audio_thread, NULL, avi_audio_thread, &audio_worker) != 0)
            goto out_decoder;
        audio_started = 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &schedule_start);

    while (!g_stop && !storage_session_lost()) { /* 暂停时也检测粘滞取消，不恢复旧会话。 */
        struct playback_packet *packet = NULL;
        int64_t position_ms;

        if (input != NULL && input->fd >= 0) { /* 播放中继续响应实体按键。 */
            struct app_key_event event; /* 播放过程中的按键事件。 */

            if (input_key_poll(input, &event) > 0) {
                if (event.key == APP_KEY_BACK &&
                    event.action == APP_KEY_ACTION_SHORT) {
                    paused = !paused; /* 中键短按：播放/暂停切换。 */
                    __atomic_store_n(&audio_paused, paused, __ATOMIC_RELEASE); /* 短按切换时同步暂停音频，避免只冻结视频。 */
                    media_clock_pause(&audio_clock, paused); /* 暂停共享媒体时钟。 */
                    if (paused)
                        pause_start_ms = media_monotonic_ms(); /* 记录暂停开始时刻。 */
                    else
                        schedule_start = timespec_add_ms(schedule_start,
                                                          media_monotonic_ms() - pause_start_ms); /* 无音频恢复时把暂停耗时加到调度起点，避免所有帧突然过期。 */
                    t113_ui_lvgl_set_playing(!paused); /* 图标随状态变化。 */
                    submit_ui_force(ui_disp, ui, last_flush); /* 立刻刷新按钮图标。 */
                } else if (event.key == APP_KEY_NEXT &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    ret = PLAYBACK_RESULT_NEXT; /* 右键切下一项。 */
                    goto out_decoder;
                } else if (event.key == APP_KEY_ENTER &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    ret = PLAYBACK_RESULT_PREV; /* 左键切上一项。 */
                    goto out_decoder;
                } else if (event.key == APP_KEY_BACK && event.action == APP_KEY_ACTION_LONG) {
                    ret = PLAYBACK_RESULT_RECORD;
                    goto out_decoder;
                }
            }
        }

        if (paused) {               /* 暂停时不再读下一包。 */
            submit_ui_if_dirty(ui_disp, ui, last_flush); /* 仍允许 UI 刷新。 */
            usleep(10000);          /* 小睡避免空转。 */
            continue;               /* 等按键恢复播放。 */
        }

        if (audio_started && __atomic_load_n(&audio_worker.done, __ATOMIC_ACQUIRE) && audio_worker.ret < 0) /* 音频任务错误不能被视频继续播放掩盖。 */
            goto out_decoder;
        if (!have_yuv && !video_eof) { /* 尚无待显示画面时才消费下一包。 */
            int popped = playback_queue_try_pop(&queue, &packet); /* 非阻塞取包，让按键、存储和界面仍能得到调度。 */
            if (popped == 0) {
                usleep(5000);
                continue;
            }
            if (popped < 0) {
                if (source.ret < 0)
                    goto out_decoder;
                decode_ret = image_convert_drain(&decoder, &pending_yuv); /* 读包 EOF 后排出 H264/H265 的重排序尾帧。 */
                if (decode_ret < 0)
                    goto out_decoder;
                if (decode_ret == 0)
                    video_eof = 1; /* 只有解码器也已排空才标记视频彻底结束。 */
                else {
                    pending_due_ms = pending_yuv.pts_ms; /* 尾帧使用解码器返回的显示时间戳。 */
                    have_yuv = 1; /* 当前输出图由调度循环持有，显示或丢弃后必须归还。 */
                }
                continue;
            }
            decode_ret = image_convert_decode_packet(&decoder, packet->data, packet->size,
                                                     packet->pts_ms, &pending_yuv);
            if (decode_ret < 0) {
                playback_packet_free(packet);
                goto out_decoder;       /* CedarC 解码失败。 */
            }
            if (decode_ret == 0) {      /* H264/H265 可能因为 B 帧重排序暂时不吐图。 */
                playback_packet_free(packet);
                continue;               /* 继续喂下一包，直到 CedarC 输出图像。 */
            }
            pending_due_ms = pending_yuv.pts_ms >= 0 ? pending_yuv.pts_ms :
                             playback_due_ms(packet->index, packet->pts_ms);
            playback_packet_free(packet);
            have_yuv = 1; /* 当前输出图由调度循环持有，显示或丢弃后必须归还。 */
        }
        if (audio_started) {
            if (!media_clock_read(&audio_clock, &position_ms)) { /* 有音频时等待真实消费时钟就绪，不以视频线程速度代替。 */
                if (__atomic_load_n(&audio_worker.done, __ATOMIC_ACQUIRE))
                    goto out_decoder;
                usleep(5000);
                continue;
            }
        } else {
            position_ms = media_monotonic_ms() - /* 无音频时使用单调时间减播放起点作为备用时钟。 */
                          ((int64_t)schedule_start.tv_sec * 1000 + schedule_start.tv_nsec / 1000000);
        }
        set_playback_progress_ms(position_ms, demuxer.duration_ms);
        if (!have_yuv) {
            if (video_eof && (!audio_started || __atomic_load_n(&audio_worker.done, __ATOMIC_ACQUIRE)))
                break;
            submit_ui_if_dirty(ui_disp, ui, last_flush);
            usleep(5000);
            continue;
        }
        if (pending_due_ms > position_ms + 5) { /* 画面尚未到显示时刻，保留输出图并短暂等待。 */
            submit_ui_if_dirty(ui_disp, ui, last_flush);
            usleep(5000);
            continue;
        }
        /* H264/H265 仍逐包解码以保留参考帧，只丢已解码的过时画面。 */
        if (position_ms - pending_due_ms > 100 && frames > 0) { /* 落后超过 100ms 时跳过过时画面，不牺牲解码器参考帧。 */
            image_convert_free_yuv(&pending_yuv); /* G2D 已完成消费或决定丢帧后归还解码输出。 */
            have_yuv = 0; /* 清除所有权标记，错误出口不能重复归还。 */
            dropped++; /* 记录被调度丢弃的解码后画面。 */
            continue;
        }
        if (g2d_display_stretch_yuv_to_dmabuf(&g2d, &pending_yuv) < 0)
            goto out_decoder;
        image_convert_free_yuv(&pending_yuv); /* G2D 已完成消费或决定丢帧后归还解码输出。 */
        have_yuv = 0; /* 清除所有权标记，错误出口不能重复归还。 */
        if (disp_layer_display_show_argb8888(&video_disp,
                                             g2d_display_get_dst_fd(&g2d),
                                             g2d_display_get_dst_width(&g2d),
                                             g2d_display_get_dst_height(&g2d)) < 0)
        {
            goto out_decoder;       /* 提交视频层失败。 */
        }
        frames++;                   /* 统计帧数。 */
        fps_meter_frame(&fps);      /* 每秒刷新一次 FPS 标签。 */
        {
            int64_t shown_ms = position_ms;
            int64_t error;
            if (audio_started)
                media_clock_read(&audio_clock, &shown_ms);
            error = shown_ms - pending_due_ms; /* 显示完成时的主时钟减图像 PTS，正值表示画面落后。 */
            if (error > max_display_error_ms)
                max_display_error_ms = error;
            if (timing_log && shown_ms - last_report_ms >= 5000) { /* 可选同步诊断最多每五秒输出一次。 */
                printf("av clock: audio=%lld video=%lld skew=%lldms frames=%u drop=%u\n",
                       (long long)shown_ms, (long long)pending_due_ms, (long long)error, frames, dropped);
                fflush(stdout);
                last_report_ms = shown_ms;
            }
        }
        submit_ui_if_dirty(ui_disp, ui, last_flush); /* 播放时仍刷新 UI。 */
    }

    ret = storage_session_lost() ? PLAYBACK_RESULT_STORAGE_LOST : PLAYBACK_RESULT_END; /* 容器任务同样区分取消与 EOF。 */

out_decoder:
    __atomic_store_n(&audio_stop, 1, __ATOMIC_RELEASE); /* 所有切换与错误出口先请求停止，不销毁运行中的资源。 */
    playback_queue_stop(&queue);    /* 同时唤醒读线程，避免切换时仍向旧队列投递。 */
    if (audio_started)              /* 等 ALSA 播放线程释放设备。 */
        pthread_join(audio_thread, NULL); /* 等待音频设备与读文件句柄归属线程自行释放。 */
    if (source_started)             /* 如果线程已经启动。 */
        pthread_join(source_thread, NULL); /* 等待线程退出。 */
    if (have_yuv)
        image_convert_free_yuv(&pending_yuv); /* G2D 已完成消费或决定丢帧后归还解码输出。 */
    printf("playback timing: displayed=%u late_drop=%u max_display_late=%lldms result=%d\n",
           frames, dropped, (long long)max_display_error_ms, ret);
    disp_layer_display_close(&video_disp); /* 先隐藏视频层，避免继续扫描 G2D dmabuf。 */
    g2d_display_close(&g2d);        /* 再关闭 G2D。 */
    image_convert_close(&decoder);  /* 最后关闭 CedarC。 */
    goto out_demux;
out_g2d:
    disp_layer_display_close(&video_disp); /* G2D 打开失败后也确保视频层关闭。 */
    g2d_display_close(&g2d);        /* 关闭 G2D。 */
    goto out_demux;
out_video_disp:
    disp_layer_display_close(&video_disp); /* 关闭视频层。 */
out_demux:
    ffmpeg_mjpeg_demuxer_close(&demuxer); /* 关闭 MKV。 */
out_queue:
    storage_bind_task(NULL);        /* 包含初始化失败路径；解绑完成后才能返回栈上下文。 */
    playback_queue_destroy(&queue); /* 销毁播放队列。 */
    printf("ui mkv frames: %u\n", frames); /* 打印播放帧数。 */
    if (clock_ready)
        media_clock_destroy(&audio_clock); /* 所有读写时钟的线程退出后再销毁锁。 */
    return ret;                     /* 返回结果。 */
}

/* 单独显示 GUI Guider 生成的 LVGL 界面，供主工程移植验证。 */
static int run_ui(void)
{
    struct disp_layer_display disp;  /* /dev/disp layer 显示上下文。 */
    struct t113_ui_lvgl ui;          /* LVGL 渲染到 dmabuf 的上下文。 */
    struct t113_ui_rect video_rect;  /* GUI 中 vedioplay 控件内容区域。 */
    uint32_t last_flush = 0;         /* 上一次已提交给 disp 的 flush 计数。 */
    uint32_t progress = 0;           /* 测试用进度条数值。 */
    unsigned int ticks = 0;          /* 5ms tick 计数，用于低频更新进度条。 */
    int ret = -1;                    /* 默认失败。 */

    memset(&disp, 0, sizeof(disp));  /* 清空显示上下文。 */
    memset(&ui, 0, sizeof(ui));      /* 清空 LVGL 上下文。 */

    kill_lvgl_demo();               /* 停掉开机自启 demo，避免抢屏幕。 */

    if (disp_layer_display_open(&disp, 800, 480) < 0) /* 打开 /dev/disp。 */
        return -1;                  /* 打开失败直接返回。 */

    if (t113_ui_lvgl_open(&ui, 800, 480) < 0) /* 初始化 LVGL 和 dmabuf。 */
        goto out_disp;              /* 失败则关闭 disp。 */

    if (t113_ui_lvgl_get_video_rect(&video_rect)) { /* 读取视频内容区。 */
        printf("ui video rect: x=%d y=%d w=%d h=%d\n",
               video_rect.x, video_rect.y, video_rect.w, video_rect.h);
    }

    if (t113_ui_lvgl_present(&ui, &disp) < 0)
        goto out_ui;                /* 首帧显示失败。 */

    last_flush = t113_ui_lvgl_get_flush_count(&ui); /* 记录首帧 flush 次数。 */
    printf("ui display  : press Ctrl+C to stop\n"); /* 提示退出方式。 */

    while (!g_stop) {               /* 持续运行 LVGL。 */
        uint32_t flush_now;         /* 当前 flush 次数。 */

        ticks++;                    /* 每轮约 5ms。 */
        if ((ticks % 20U) == 0U) {  /* 约 100ms 更新一次进度条。 */
            progress++;             /* 进度自增。 */
            if (progress > 100U)    /* 到 100 后回到 0。 */
                progress = 0U;      /* 循环进度。 */
            t113_ui_lvgl_set_progress(progress, 100); /* 写入 LVGL slider。 */
        }

        t113_ui_lvgl_tick(&ui);     /* 运行一次 LVGL 定时器。 */
        flush_now = t113_ui_lvgl_get_flush_count(&ui); /* 读取刷新计数。 */
        if (flush_now != last_flush) { /* 有新画面才重新提交图层配置。 */
            last_flush = flush_now; /* 更新已提交计数。 */
            if (t113_ui_lvgl_present(&ui, &disp) < 0)
                goto out_ui;        /* 提交失败则退出。 */
        }

        usleep(5000);               /* 小睡降低 CPU 占用。 */
    }

    ret = 0;                        /* Ctrl+C 正常退出。 */

out_ui:
    printf("ui flushes  : %u\n", t113_ui_lvgl_get_flush_count(&ui));
    disp_layer_display_hide(&disp);
    t113_ui_lvgl_close(&ui);        /* 释放 LVGL ION 缓冲。 */
out_disp:
    disp_layer_display_close(&disp);/* 关闭显示图层。 */
    return ret;                     /* 返回结果。 */
}

/* 命令行测试：在 UI 的 vedioplay 区域播放 AVI/MKV 容器视频。 */
static int run_ui_mkv_video(const char *path)
{
    struct disp_layer_display ui_disp; /* UI 背景图层。 */
    struct t113_ui_lvgl ui;          /* LVGL UI 上下文。 */
    uint32_t last_flush = 0;         /* UI 上次提交的 flush 计数。 */
    enum playback_result play_result; /* 播放结果。 */
    int ret = -1;                    /* 默认失败。 */

    memset(&ui_disp, 0, sizeof(ui_disp)); /* 清空 UI 图层。 */
    memset(&ui, 0, sizeof(ui));      /* 清空 UI 上下文。 */

    kill_lvgl_demo();               /* 停掉开机自启 demo。 */
    hide_video_overlay_layer();     /* 清掉可能残留的视频叠加层。 */

    if (disp_layer_display_open_ex(&ui_disp, 800, 480,
                                   1, 0, 10,
                                   0, 0, 800, 480) < 0)
        return -1;                  /* UI 图层打开失败。 */

    if (t113_ui_lvgl_open(&ui, 800, 480) < 0)
        goto out_disp;              /* LVGL 初始化失败。 */

    t113_ui_lvgl_set_screen_title("T113Camera"); /* 设置标题。 */
    t113_ui_lvgl_set_title("VIDEO", path); /* 显示当前文件。 */
    t113_ui_lvgl_set_playing(true); /* 播放图标切成暂停态。 */
    if (t113_ui_lvgl_present(&ui, &ui_disp) < 0)
        goto out_ui;                /* 首帧 UI 显示失败。 */
    last_flush = t113_ui_lvgl_get_flush_count(&ui); /* 记录首帧。 */

    play_result = play_mkv_on_current_ui(&ui_disp, &ui, NULL,
                                         path, &last_flush); /* 播放 MKV。 */
    if (play_result == PLAYBACK_RESULT_END)
        ret = 0;                    /* 播放到结束视为成功。 */

out_ui:
    hide_video_overlay_layer();     /* 退出命令行测试时隐藏视频层。 */
    t113_ui_lvgl_set_playing(false);/* 图标恢复播放态。 */
    disp_layer_display_hide(&ui_disp);
    t113_ui_lvgl_close(&ui);        /* 释放 LVGL。 */
out_disp:
    disp_layer_display_close(&ui_disp); /* 关闭 UI 图层。 */
    return ret;                     /* 返回结果。 */
}

/* 命令行测试：在 UI 中播放 WAV 音频文件。 */
static int run_ui_wav(const char *path)
{
    struct disp_layer_display ui_disp; /* UI 背景图层。 */
    struct t113_ui_lvgl ui;          /* LVGL UI 上下文。 */
    uint32_t last_flush = 0;         /* UI 上次提交的 flush 计数。 */
    enum playback_result play_result; /* 播放结果。 */
    int ret = -1;                    /* 默认失败。 */

    memset(&ui_disp, 0, sizeof(ui_disp)); /* 清空 UI 图层。 */
    memset(&ui, 0, sizeof(ui));      /* 清空 UI 上下文。 */

    kill_lvgl_demo();               /* 停掉开机自启 demo。 */
    hide_video_overlay_layer();     /* 清掉可能残留的视频叠加层。 */

    if (disp_layer_display_open_ex(&ui_disp, 800, 480,
                                   1, 0, 10,
                                   0, 0, 800, 480) < 0)
        return -1;                  /* UI 图层打开失败。 */

    if (t113_ui_lvgl_open(&ui, 800, 480) < 0)
        goto out_disp;              /* LVGL 初始化失败。 */

    t113_ui_lvgl_set_screen_title("T113Camera"); /* 设置标题。 */
    t113_ui_lvgl_set_title("WAV", path); /* 显示当前文件。 */
    t113_ui_lvgl_set_playing(true); /* 播放图标切成暂停态。 */
    if (t113_ui_lvgl_present(&ui, &ui_disp) < 0)
        goto out_ui;                /* 首帧 UI 显示失败。 */
    last_flush = t113_ui_lvgl_get_flush_count(&ui); /* 记录首帧。 */

    play_result = play_ffmpeg_audio_on_current_ui(&ui_disp, &ui, NULL,
                                         path, &last_flush); /* 播放 WAV。 */
    if (play_result == PLAYBACK_RESULT_END)
        ret = 0;                    /* 播放到结束视为成功。 */

out_ui:
    t113_ui_lvgl_set_playing(false);/* 图标恢复播放态。 */
    disp_layer_display_hide(&ui_disp);
    t113_ui_lvgl_close(&ui);        /* 释放 LVGL。 */
out_disp:
    disp_layer_display_close(&ui_disp); /* 关闭 UI 图层。 */
    return ret;                     /* 返回结果。 */
}

/* 命令行测试：在 UI 中播放 AAC/MP3 等压缩音频文件。 */
static int run_ui_ffmpeg_audio(const char *path)
{
    struct disp_layer_display ui_disp; /* UI 背景图层。 */
    struct t113_ui_lvgl ui;          /* LVGL UI 上下文。 */
    uint32_t last_flush = 0;         /* UI 上次提交的 flush 计数。 */
    enum playback_result play_result; /* 播放结果。 */
    int ret = -1;                    /* 默认失败。 */

    memset(&ui_disp, 0, sizeof(ui_disp)); /* 清空 UI 图层。 */
    memset(&ui, 0, sizeof(ui));      /* 清空 UI 上下文。 */

    kill_lvgl_demo();               /* 停掉开机自启 demo。 */
    hide_video_overlay_layer();     /* 清掉可能残留的视频叠加层。 */

    if (disp_layer_display_open_ex(&ui_disp, 800, 480,
                                   1, 0, 10,
                                   0, 0, 800, 480) < 0)
        return -1;                  /* UI 图层打开失败。 */

    if (t113_ui_lvgl_open(&ui, 800, 480) < 0)
        goto out_disp;              /* LVGL 初始化失败。 */

    t113_ui_lvgl_set_screen_title("T113Camera"); /* 设置标题。 */
    t113_ui_lvgl_set_title("AUDIO", path); /* 显示当前文件。 */
    t113_ui_lvgl_set_playing(true); /* 播放图标切成暂停态。 */
    if (t113_ui_lvgl_present(&ui, &ui_disp) < 0)
        goto out_ui;                /* 首帧 UI 显示失败。 */
    last_flush = t113_ui_lvgl_get_flush_count(&ui); /* 记录首帧。 */

    play_result = play_ffmpeg_audio_on_current_ui(&ui_disp, &ui, NULL,
                                                  path, &last_flush); /* 播放压缩音频。 */
    if (play_result == PLAYBACK_RESULT_END)
        ret = 0;                    /* 播放到结束视为成功。 */

out_ui:
    t113_ui_lvgl_set_playing(false);/* 图标恢复播放态。 */
    disp_layer_display_hide(&ui_disp);
    t113_ui_lvgl_close(&ui);        /* 释放 LVGL。 */
out_disp:
    disp_layer_display_close(&ui_disp); /* 关闭 UI 图层。 */
    return ret;                     /* 返回结果。 */
}

/* 在 LVGL 界面上播放 MJPEG 裸流文件，视频显示到 vedioplay 内容区域。 */
static int run_ui_video(const char *path)
{
    struct disp_layer_display ui_disp; /* UI 背景图层。 */
    struct disp_layer_display video_disp; /* 视频叠加图层。 */
    struct t113_ui_lvgl ui;          /* LVGL UI 上下文。 */
    struct t113_ui_rect video_rect;  /* vedioplay 内容区域。 */
    struct cedar_decoder decoder;    /* CedarC MJPG 解码器。 */
    struct g2d_display g2d;          /* G2D 缩放和格式转换。 */
    unsigned char *file_data = NULL; /* MJPEG 文件内容。 */
    size_t file_size = 0;            /* MJPEG 文件大小。 */
    size_t offset = 0;               /* 当前解析偏移。 */
    unsigned int frames = 0;         /* 已播放帧数。 */
    unsigned int total_frames = 0;   /* 文件总帧数，用于计算进度。 */
    unsigned int src_width, src_height;
    uint32_t last_flush = 0;         /* UI 上次提交的 flush 计数。 */
    int ret = -1;                    /* 默认失败。 */

    memset(&ui_disp, 0, sizeof(ui_disp)); /* 清空 UI 图层。 */
    memset(&video_disp, 0, sizeof(video_disp)); /* 清空视频图层。 */
    memset(&ui, 0, sizeof(ui));      /* 清空 UI 上下文。 */
    memset(&decoder, 0, sizeof(decoder)); /* 清空 decoder。 */
    memset(&g2d, 0, sizeof(g2d));    /* 清空 G2D。 */
    g2d.fd = -1;                    /* 标记 G2D 尚未打开。 */

    kill_lvgl_demo();               /* 停掉开机自启 demo。 */

    if (read_file_all(path, &file_data, &file_size) < 0) /* 读取 MJPEG 文件。 */
        return -1;                  /* 读取失败。 */
    printf("ui video    : %s bytes=%lu\n", path, (unsigned long)file_size);
    if (jpeg_dimensions(file_data, file_size, &src_width, &src_height) < 0)
        goto out_file;
    total_frames = count_mjpeg_frames(file_data, file_size); /* 统计完整 JPEG 帧。 */
    if (total_frames == 0U) {       /* 没有可播放帧。 */
        fprintf(stderr, "no jpeg frame found in %s\n", path);
        goto out_file;              /* 直接退出。 */
    }

    if (disp_layer_display_open_ex(&ui_disp, 800, 480,
                                   1, 0, 10,
                                   0, 0, 800, 480) < 0) /* UI 使用底层。 */
        goto out_file;              /* 打开失败。 */

    if (t113_ui_lvgl_open(&ui, 800, 480) < 0) /* 初始化 LVGL UI。 */
        goto out_ui_disp;           /* 初始化失败。 */

    if (!t113_ui_lvgl_get_video_rect(&video_rect)) { /* 获取视频窗口。 */
        fprintf(stderr, "get UI video rect failed\n");
        goto out_ui;                /* 无法定位视频区域。 */
    }
    printf("ui video rect: x=%d y=%d w=%d h=%d\n",
           video_rect.x, video_rect.y, video_rect.w, video_rect.h);

    if (t113_ui_lvgl_present(&ui, &ui_disp) < 0)
        goto out_ui;                /* 显示 UI 首帧失败。 */
    last_flush = t113_ui_lvgl_get_flush_count(&ui); /* 记录 UI flush。 */
    set_playback_progress(0, total_frames); /* 进度从 0ms 开始。 */

    if (disp_layer_display_open_ex(&video_disp, 800, 480,
                                   1, 1, 12,
                                   video_rect.x, video_rect.y,
                                   (unsigned int)video_rect.w,
                                   (unsigned int)video_rect.h) < 0) /* 视频层在 UI 上方。 */
        goto out_ui;                /* 打开失败。 */

    if (g2d_display_open_sized(&g2d, src_width, src_height,
                               (unsigned int)video_rect.w,
                               (unsigned int)video_rect.h) < 0) /* G2D 输出到视频窗口大小。 */
        goto out_video_disp;        /* 打开失败。 */

    if (image_convert_open(&decoder, src_width, src_height) < 0)
        goto out_g2d;               /* 打开失败。 */

    while (!g_stop && !storage_session_lost()) { /* 命令行 JPEG/裸流同样响应拔盘。 */
        const unsigned char *frame_data; /* 当前 JPEG 帧数据。 */
        size_t frame_size;          /* 当前 JPEG 帧长度。 */
        struct yuv_image yuv;       /* 解码后的 YUV 图像。 */
        uint32_t flush_now;         /* 当前 UI flush 计数。 */

        if (!find_next_jpeg_frame(file_data, file_size, &offset,
                                  &frame_data, &frame_size)) /* 找下一帧。 */
            break;                  /* 文件播放结束。 */

        memset(&yuv, 0, sizeof(yuv)); /* 清空 YUV 结构。 */
        if (image_convert_decode_mjpeg(&decoder, frame_data, frame_size, &yuv) < 0)
            goto out_decoder;       /* 解码失败。 */

        if (g2d_display_stretch_yuv_to_dmabuf(&g2d, &yuv) < 0) { /* G2D 转换缩放。 */
            image_convert_free_yuv(&yuv); /* 释放 YUV。 */
            goto out_decoder;       /* 显示失败。 */
        }
        image_convert_free_yuv(&yuv); /* 释放 CedarC 输出帧。 */

        if (disp_layer_display_show_argb8888(&video_disp,
                                             g2d_display_get_dst_fd(&g2d),
                                             g2d_display_get_dst_width(&g2d),
                                             g2d_display_get_dst_height(&g2d)) < 0)
            goto out_decoder;       /* 提交视频层失败。 */

        frames++;                   /* 统计播放帧数。 */
        set_playback_progress(frames, total_frames); /* 按播放时长更新进度。 */
        t113_ui_lvgl_tick(&ui);     /* 处理 UI 更新。 */
        flush_now = t113_ui_lvgl_get_flush_count(&ui); /* 查询 UI 是否刷新。 */
        if (flush_now != last_flush) { /* 如果 UI 有变化。 */
            last_flush = flush_now; /* 更新计数。 */
            t113_ui_lvgl_present(&ui, &ui_disp);
        }

        usleep(33333);              /* 第一版按约 30fps 播放。 */
    }

    ret = 0;                        /* 播放到文件结束视为成功。 */

out_decoder:
    disp_layer_display_close(&video_disp); /* 先隐藏视频图层。 */
    g2d_display_close(&g2d);        /* 再释放 G2D dmabuf。 */
    image_convert_close(&decoder);  /* 最后关闭 CedarC。 */
    goto out_ui;
out_g2d:
    disp_layer_display_close(&video_disp); /* 确保视频图层关闭。 */
    g2d_display_close(&g2d);        /* 关闭 G2D。 */
    goto out_ui;
out_video_disp:
    disp_layer_display_close(&video_disp); /* 关闭视频图层。 */
out_ui:
    printf("ui video frames: %u\n", frames);
    disp_layer_display_hide(&ui_disp);
    t113_ui_lvgl_close(&ui);        /* 关闭 UI。 */
out_ui_disp:
    disp_layer_display_close(&ui_disp); /* 关闭 UI 图层。 */
out_file:
    free(file_data);                /* 释放文件缓冲。 */
    return ret;                     /* 返回结果。 */
}

/* 默认播放模式：显示播放列表，用三个实体按键选择并播放媒体。 */
/* 应用协调层：旧任务收尾完成后再确认新盘；监控模块完全不依赖此函数。 */
static int sync_storage_ui(struct disp_layer_display *disp, struct t113_ui_lvgl *ui,
                           struct record_preview *record, struct still_preview *still,
                           struct media_list *list, size_t *selected, int *record_mode,
                           uint64_t *generation, uint32_t *last_flush)
{
    struct storage_snapshot storage; /* 只使用锁内复制出来的一致快照。 */
    storage_monitor_snapshot(&g_storage, &storage); /* 获取设备与挂载身份。 */
    if (storage.generation == *generation && !storage_session_lost())
        return 1; /* 没有变化，不重扫目录、不重开解码器。 */
    if (*record_mode) { /* 原相机任务必须先退出，不能跨会话继续保存。 */
        if (record->recording)
            record_writer_abort(&record->sink); /* 通知写线程失败，不跨线程关闭 muxer。 */
        record_preview_close(record); /* join 音频与写线程，然后关闭摄像头及预览。 */
        *record_mode = 0; /* 换盘统一回到播放列表，不自动恢复录像。 */
    }
    still_preview_close(still); /* 关闭旧照片及其显示图层。 */
    hide_video_overlay_layer(); /* 清除旧视频，保留 UI 层。 */
    t113_ui_lvgl_set_audio_visualizer(false); /* 清除旧音频条。 */
    t113_ui_lvgl_set_playing(false); /* 恢复未播放按钮。 */
    t113_ui_lvgl_set_rec_led(0); /* 离开相机模式后关灯。 */
    t113_ui_lvgl_set_screen_title("VEDIO PLAYER"); /* 只改文本，不改模板样式。 */
    t113_ui_lvgl_set_progress(0, 100); /* 旧文件进度不带入新盘。 */
    t113_ui_lvgl_set_fps(0); /* 清零旧帧率。 */
    if (storage_monitor_adopt(&g_storage, storage.generation) < 0)
        return 0; /* 清理期间再次插拔，下轮使用最新状态。 */
    *generation = storage.generation; /* 记录应用已完成清理的会话。 */
    snprintf(g_storage_root, sizeof(g_storage_root), "%s", storage.root); /* 缺盘为空，绝不回退到 flash。 */
    *selected = 0; /* 新列表从第一项开始。 */
    refresh_media_list_ui(list, selected); /* 扫描实际挂载目录，缺盘清空列表。 */
    update_playback_selection_preview(disp, ui, still, list, *selected, last_flush); /* 容器视频首帧预览保持关闭。 */
    if (!storage.state)
        t113_ui_lvgl_set_video_message("USB NOT READY"); /* 提示无法播放或录制。 */
    if (storage.state == STORAGE_READ_ONLY)
        t113_ui_lvgl_set_usb_status("USB READ ONLY"); /* 允许播放，禁止录制。 */
    submit_ui_force(disp, ui, last_flush); /* LVGL 只在主线程执行。 */
    printf("usb state   : generation=%llu state=%d root=%s\n",
           (unsigned long long)*generation, storage.state, USB_ROOT); /* 仅变化时记录日志。 */
    return !storage_session_lost(); /* 扫描期间又拔盘时不处理旧按键。 */
}

static int run_player_mode(void)
{
    struct disp_layer_display ui_disp; /* UI 背景图层。 */
    struct t113_ui_lvgl ui;          /* LVGL UI 上下文。 */
    struct input_key input;          /* 三按键输入。 */
    struct media_list list;          /* U 盘媒体列表。 */
    struct record_preview record_preview; /* 录制模式实时预览。 */
    struct still_preview still_preview; /* 播放列表第一帧预览。 */
    struct t113_ui_rect video_rect;  /* UI 视频框内容区域。 */
    struct fps_meter record_fps;     /* 录制模式预览实际帧率。 */
    size_t selected = 0;             /* 当前高亮条目。 */
    size_t record_selected = 0;      /* 录制模式当前选择的保存文件名。 */
    uint32_t last_flush = 0;         /* UI 上次提交的 flush 计数。 */
    int video_rect_ready = 0;        /* 是否成功取得视频框区域。 */
    int input_ready = 0;             /* 输入设备是否打开成功。 */
    int record_mode = 0;             /* 当前是否处于录制模式占位状态。 */
    int photo_mode = 0;              /* 长按 NEXT 切换，短按中键拍照。 */
    uint64_t storage_generation = 0; /* 主线程最后完成收尾并确认的存储会话。 */
    int ret = -1;                    /* 默认失败。 */

    memset(&ui_disp, 0, sizeof(ui_disp)); /* 清空 UI 图层。 */
    memset(&ui, 0, sizeof(ui));      /* 清空 LVGL 上下文。 */
    memset(&input, 0, sizeof(input));/* 清空输入上下文。 */
    memset(&record_preview, 0, sizeof(record_preview)); /* 清空录制预览上下文。 */
    memset(&still_preview, 0, sizeof(still_preview)); /* 清空第一帧预览上下文。 */
    input.fd = -1;                  /* 标记输入设备未打开。 */
    record_preview.cap.fd = -1;     /* 标记摄像头未打开。 */
    record_preview.g2d.fd = -1;     /* 标记 G2D 未打开。 */
    still_preview.g2d.fd = -1;      /* 标记 G2D 未打开。 */
    fps_meter_reset(&record_fps);    /* 初始化录制预览 FPS 统计。 */
    media_list_init(&list);         /* 清空媒体列表。 */

    kill_lvgl_demo();               /* 停掉开机 LVGL demo，避免抢屏幕。 */
    hide_video_overlay_layer();     /* 清掉可能残留的视频叠加层。 */

    if (disp_layer_display_open_ex(&ui_disp, 800, 480,
                                   1, 0, 10,
                                   0, 0, 800, 480) < 0)
        return -1;                  /* UI 图层打开失败。 */

    if (t113_ui_lvgl_open(&ui, 800, 480) < 0)
        goto out_disp;              /* LVGL 初始化失败。 */
    video_rect_ready = t113_ui_lvgl_get_video_rect(&video_rect); /* 获取视频框。 */

    refresh_media_list_ui(&list, &selected); /* 扫描 U 盘并填充 GUI 播放列表。 */
    t113_ui_lvgl_set_screen_title("VEDIO PLAYER"); /* 播放模式顶部标题。 */
    t113_ui_lvgl_set_playing(false); /* 默认未播放。 */
    t113_ui_lvgl_set_rec_led(0);    /* 播放模式下 REC 灯熄灭。 */
    t113_ui_lvgl_set_fps(0);        /* 启动时 FPS 清零。 */

    if (t113_ui_lvgl_present(&ui, &ui_disp) < 0)
        goto out_ui;                /* 首帧 UI 显示失败。 */
    last_flush = t113_ui_lvgl_get_flush_count(&ui); /* 记录首帧。 */
    update_playback_selection_preview(&ui_disp, &ui, &still_preview,
                                      &list, selected, &last_flush); /* 启动后预览第一项。 */

    if (input_key_open(&input, INPUT_DEVICE) == 0) { /* 打开三按键。 */
        input_ready = 1;            /* 输入可用。 */
        printf("input       : %s\n", INPUT_DEVICE);
    } else {
        printf("input       : %s open failed, UI still running\n", INPUT_DEVICE);
    }

    printf("mode        : player list, Ctrl+C to stop\n");
    while (!g_stop) {               /* 主事件循环。 */
        struct app_key_event event; /* 当前按键事件。 */

        if (!sync_storage_ui(&ui_disp, &ui, &record_preview, &still_preview,
                             &list, &selected, &record_mode, &storage_generation, &last_flush))
            continue; /* 清理期间再次换盘：取新快照，暂不响应旧列表按键。 */
        if (input_ready && input_key_poll(&input, &event) > 0) { /* 有按键。 */
            if (!record_mode) {     /* 播放列表模式。 */
                if (event.key == APP_KEY_NEXT &&
                    event.action == APP_KEY_ACTION_SHORT && list.count > 0) {
                    selected = (selected + 1U) % list.count; /* 下一项。 */
                    t113_ui_lvgl_playlist_select((uint32_t)selected);
                    update_playback_selection_preview(&ui_disp, &ui, &still_preview,
                                                      &list, selected, &last_flush);
                } else if (event.key == APP_KEY_ENTER &&
                           event.action == APP_KEY_ACTION_SHORT && list.count > 0) {
                    selected = (selected == 0U) ? list.count - 1U : selected - 1U; /* 上一项。 */
                    t113_ui_lvgl_playlist_select((uint32_t)selected);
                    update_playback_selection_preview(&ui_disp, &ui, &still_preview,
                                                      &list, selected, &last_flush);
                } else if (event.key == APP_KEY_BACK &&
                           event.action == APP_KEY_ACTION_SHORT && list.count > 0) {
                    const struct media_item *item = &list.items[selected]; /* 当前文件。 */

                    if (item->playable) { /* 支持裸 MJPEG、AVI/MKV 和 WAV。 */
                        enum playback_result play_result; /* 播放结束原因。 */

                        still_preview_close(&still_preview); /* 播放前关闭静态预览层。 */
                        t113_ui_lvgl_set_playing(true);
                        t113_ui_lvgl_set_video_message("");
                        submit_ui_force(&ui_disp, &ui, &last_flush);
                        if (item->type == MEDIA_TYPE_JPEG) {
                            play_result = PLAYBACK_RESULT_END;
                        } else if (item->type == MEDIA_TYPE_MKV ||
                            item->type == MEDIA_TYPE_AVI ||
                            item->type == MEDIA_TYPE_MP4) /* 容器文件：FFmpeg 解封装。 */
                            play_result = play_mkv_on_current_ui(&ui_disp, &ui,
                                                                 input_ready ? &input : NULL,
                                                                 item->path, &last_flush);
                        else if (item->type == MEDIA_TYPE_WAV) /* 音频文件：ALSA 播放。 */
                            play_result = play_ffmpeg_audio_on_current_ui(&ui_disp, &ui,
                                                                 input_ready ? &input : NULL,
                                                                 item->path, &last_flush);
                        else if (item->type == MEDIA_TYPE_AAC ||
                                 item->type == MEDIA_TYPE_MP3) /* 压缩音频：FFmpeg 解码后 ALSA 播放。 */
                            play_result = play_ffmpeg_audio_on_current_ui(&ui_disp, &ui,
                                                                         input_ready ? &input : NULL,
                                                                         item->path, &last_flush);
                        else                               /* 历史测试文件：裸 MJPEG。 */
                            play_result = play_mjpeg_on_current_ui(&ui_disp, &ui,
                                                                   input_ready ? &input : NULL,
                                                                   item->path, &last_flush);
                        if (storage_session_lost()) continue; /* 先进入统一换盘收尾，不再渲染旧列表或旧照片。 */
                        hide_video_overlay_layer();
                        t113_ui_lvgl_set_playing(false);
                        t113_ui_lvgl_set_fps(0);
                        t113_ui_lvgl_set_progress(0, 100);
                        if (play_result == PLAYBACK_RESULT_RECORD)
                            goto enter_record_mode;
                        if (play_result == PLAYBACK_RESULT_NEXT && list.count > 0) {
                            selected = (selected + 1U) % list.count; /* 播放中切下一项。 */
                            t113_ui_lvgl_playlist_select((uint32_t)selected);
                        } else if (play_result == PLAYBACK_RESULT_PREV && list.count > 0) {
                            selected = (selected == 0U) ? list.count - 1U : selected - 1U; /* 播放中切上一项。 */
                            t113_ui_lvgl_playlist_select((uint32_t)selected);
                        }
                        update_playback_selection_preview(&ui_disp, &ui, &still_preview,
                                                          &list, selected, &last_flush);
                        if (play_result == PLAYBACK_RESULT_ERROR) {
                            t113_ui_lvgl_set_video_message("PLAY ERROR");
                            submit_ui_force(&ui_disp, &ui, &last_flush);
                        }
                    } else {
                        still_preview_close(&still_preview);
                        hide_video_overlay_layer();
                        t113_ui_lvgl_set_audio_visualizer(false);
                        t113_ui_lvgl_set_video_message("NO SUPPORT");
                        submit_ui_force(&ui_disp, &ui, &last_flush);
                    }
                } else if (event.key == APP_KEY_BACK &&
                           event.action == APP_KEY_ACTION_LONG) {
enter_record_mode:
                    if (storage_session_lost() || !media_storage_ready(USB_ROOT, 1)) {
                        t113_ui_lvgl_set_video_message("USB NOT READY");
                        submit_ui_force(&ui_disp, &ui, &last_flush);
                        continue;
                    }
                    record_mode = 1; /* 进入录制模式占位状态。 */
                    photo_mode = 0;
                    record_selected = 0; /* 默认选择 record1。 */
                    still_preview_close(&still_preview); /* 进入录制模式前关闭静态预览层。 */
                    hide_video_overlay_layer(); /* 先清掉播放模式可能留下的视频图层。 */
                    usleep(200000); /* 等待上一套 CedarC/G2D/ION 资源完全释放，避免打开摄像头预览时 OOM。 */
                    fill_record_names_ui(record_selected); /* 左侧列表切成录制文件名。 */
                    t113_ui_lvgl_set_screen_title("Camera"); /* 顶部 TITLE 进入相机模式。 */
                    t113_ui_lvgl_set_title(g_record_names[record_selected], "Ready");
                    t113_ui_lvgl_set_usb_status("naming"); /* 录制模式下 usbsta 显示命名状态。 */
                    t113_ui_lvgl_set_audio_visualizer(false);
                    t113_ui_lvgl_set_video_message("OPEN CAMERA...");
                    t113_ui_lvgl_set_playing(false); /* 录制预览不是播放状态。 */
                    t113_ui_lvgl_set_rec_led(255); /* 录制模式待机时 REC 灯常亮。 */
                    t113_ui_lvgl_set_fps(0); /* 进入录制模式重新统计预览 FPS。 */
                    fps_meter_reset(&record_fps); /* 重置录制预览 FPS 窗口。 */
                    t113_ui_lvgl_set_progress(0, 100); /* 清空上一段视频播放进度。 */
                    submit_ui_force(&ui_disp, &ui, &last_flush); /* 先把录制 UI 刷到屏幕。 */
                    if (video_rect_ready &&
                        record_preview_open(&record_preview, &video_rect) == 0) {
                        t113_ui_lvgl_set_audio_visualizer(false);
                        t113_ui_lvgl_set_video_message("");
                        record_preview_step(&record_preview); /* 马上送第一帧到 vedioplay。 */
                    } else {
                        t113_ui_lvgl_set_video_message("CAMERA ERROR");
                    }
                    submit_ui_force(&ui_disp, &ui, &last_flush);
                }
            } else {                /* 录制模式占位状态。 */
                if (event.key == APP_KEY_BACK &&
                    event.action == APP_KEY_ACTION_LONG) {
                    int save_result = record_preview_stop_record(&record_preview);
                    record_mode = 0; /* 返回播放列表。 */
                    record_preview_close(&record_preview);
                    hide_video_overlay_layer();
                    t113_ui_lvgl_set_screen_title("VEDIO PLAYER");
                    t113_ui_lvgl_set_rec_led(0); /* 离开录制模式后 REC 灯熄灭。 */
                    t113_ui_lvgl_set_fps(0);     /* 返回播放列表时清空 FPS。 */
                    t113_ui_lvgl_set_audio_visualizer(false);
                    usleep(300000); /* 避免刚写完文件就立刻解封装/扫描。 */
                    refresh_media_list_ui(&list, &selected);
                    t113_ui_lvgl_set_playing(false);
                    update_playback_selection_preview(&ui_disp, &ui, &still_preview,
                                                      &list, selected, &last_flush);
                    if (save_result < 0) {
                        t113_ui_lvgl_set_video_message("SAVE ERROR");
                        submit_ui_force(&ui_disp, &ui, &last_flush);
                    }
                } else if (event.key == APP_KEY_NEXT && event.action == APP_KEY_ACTION_LONG) {
                    if (!record_preview.recording) {
                        photo_mode = !photo_mode;
                        t113_ui_lvgl_set_title(g_record_names[record_selected], photo_mode ? "Photo" : "Video");
                        printf("camera mode : %s\n", photo_mode ? "photo" : "video");
                        submit_ui_force(&ui_disp, &ui, &last_flush);
                    }
                } else if (event.key == APP_KEY_NEXT &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    if (record_preview.recording) {
                        t113_ui_lvgl_set_video_message("STOP REC FIRST");
                    } else {
                        size_t record_count = sizeof(g_record_names) / sizeof(g_record_names[0]);

                        record_selected = (record_selected + 1U) % record_count;
                        fill_record_names_ui(record_selected);
                        t113_ui_lvgl_set_title(g_record_names[record_selected], "Ready");
                        t113_ui_lvgl_set_video_message("");
                    }
                    submit_ui_force(&ui_disp, &ui, &last_flush);
                } else if (event.key == APP_KEY_ENTER &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    if (record_preview.recording) {
                        t113_ui_lvgl_set_video_message("STOP REC FIRST");
                    } else {
                        size_t record_count = sizeof(g_record_names) / sizeof(g_record_names[0]);

                        record_selected = (record_selected == 0U) ?
                                          record_count - 1U :
                                          record_selected - 1U;
                        fill_record_names_ui(record_selected);
                        t113_ui_lvgl_set_title(g_record_names[record_selected], "Ready");
                        t113_ui_lvgl_set_video_message("");
                    }
                    submit_ui_force(&ui_disp, &ui, &last_flush);
                } else if (event.key == APP_KEY_BACK &&
                           event.action == APP_KEY_ACTION_SHORT) {
                    if (!record_preview.opened) {
                        t113_ui_lvgl_set_video_message("CAMERA ERROR");
                    } else if (photo_mode && !record_preview.recording) {
                        int saved = record_preview_photo(&record_preview, g_record_names[record_selected]);
                        t113_ui_lvgl_set_title(g_record_names[record_selected], saved == 0 ? "Photo saved" : "Save error");
                    } else if (!record_preview.recording) {
                        if (record_preview_start_record(&record_preview,
                                                        g_record_names[record_selected]) == 0) {
                            t113_ui_lvgl_set_video_message("REC");
                            t113_ui_lvgl_set_playing(true);
                            t113_ui_lvgl_set_rec_led(record_breath_brightness());
                            t113_ui_lvgl_set_title(g_record_names[record_selected], "Recording");
                            printf("ui record   : start -> %s\n",
                                   record_preview.record_path);
                        } else {
                            t113_ui_lvgl_set_video_message("REC ERROR");
                        }
                    } else {
                        if (record_preview_stop_record(&record_preview) == 0) {
                            unsigned int saved_frames = record_preview.writer.frames;
                            char message[64];

                            snprintf(message, sizeof(message), "SAVED %u FRAMES",
                                     saved_frames);
                            t113_ui_lvgl_set_video_message(message);
                            t113_ui_lvgl_set_playing(false);
                            t113_ui_lvgl_set_rec_led(255); /* 仍在录制模式，停止后恢复常亮。 */
                            t113_ui_lvgl_set_title(g_record_names[record_selected], "Saved");
                            printf("ui record   : stop frames=%u path=%s\n",
                                   saved_frames, record_preview.record_path);
                        } else {
                            t113_ui_lvgl_set_video_message("SAVE ERROR");
                            t113_ui_lvgl_set_playing(false);
                            t113_ui_lvgl_set_rec_led(255);
                        }
                    }
                    submit_ui_force(&ui_disp, &ui, &last_flush);
                }
            }
        }

        if (record_mode && record_preview.opened) { /* 录制模式下持续预览摄像头。 */
            if (record_preview_step(&record_preview) < 0) {
                if (g_stop)
                    break;          /* Ctrl+C 打断 select 属于正常停止，继续写尾。 */
                if (record_preview.recording)
                    record_writer_abort(&record_preview.sink);
                record_preview_close(&record_preview);
                t113_ui_lvgl_set_playing(false);
                t113_ui_lvgl_set_video_message("CAMERA ERROR");
                t113_ui_lvgl_set_rec_led(255);
                submit_ui_force(&ui_disp, &ui, &last_flush);
            } else {
                fps_meter_frame(&record_fps); /* 统计录制预览真实 FPS。 */
                if (record_preview.recording) /* 录制中 REC 灯做呼吸效果。 */
                    t113_ui_lvgl_set_rec_led(record_breath_brightness());
            }
        }

        if (submit_ui_if_dirty(&ui_disp, &ui, &last_flush) < 0) /* 刷新 UI 图层。 */
            goto out_input;         /* 显示失败退出。 */
        usleep(5000);               /* 降低 CPU 占用。 */
    }

    ret = 0;                        /* Ctrl+C 正常退出。 */

out_input:
    still_preview_close(&still_preview);
    if (record_preview_stop_record(&record_preview) < 0)
        ret = -1;                   /* 退出时保存失败也必须反映到进程退出码。 */
    record_preview_close(&record_preview);
    if (input_ready)                /* 输入打开过就关闭。 */
        input_key_close(&input);
out_ui:
    disp_layer_display_hide(&ui_disp);
    t113_ui_lvgl_close(&ui);        /* 释放 LVGL/ION 资源。 */
out_disp:
    disp_layer_display_close(&ui_disp); /* 关闭 UI 图层。 */
    return ret;                     /* 返回执行结果。 */
}

/* 把字符串解析成 1~59 秒的录制时长。 */
static int parse_record_seconds(const char *text, int *seconds)
{
    char *end = NULL;               /* strtol 停止解析的位置。 */
    long value;                     /* strtol 的长整型结果。 */

    errno = 0;                      /* 调用前清零 errno。 */
    value = strtol(text, &end, 10); /* 按 10 进制解析输入。 */
    if (errno != 0 || end == text || *end != '\0') /* 检查非数字或溢出。 */
        return -1;                  /* 参数非法。 */
    if (value <= 0 || value >= 60)  /* 只允许 1~59 秒。 */
        return -1;                  /* 参数非法。 */
    if (value == LONG_MIN || value == LONG_MAX) /* 保险处理边界值。 */
        return -1;                  /* 参数非法。 */

    *seconds = (int)value;          /* 输出合法秒数。 */
    return 0;                       /* 解析成功。 */
}

/* 杀掉开机自启的 LVGL/demo 程序，避免它继续占用/刷新显示设备。 */
static void kill_lvgl_demo(void)
{
    int status;                     /* system 调用返回值。 */

    status = system("for p in demo lvgl_demo lvgl_ui bootanimation; do "
                    "pidof $p >/dev/null 2>&1 && killall $p >/dev/null 2>&1 && echo $p; "
                    "done");
    if (status == -1) {             /* system 本身执行失败。 */
        perror("system kill lvgl demo"); /* 打印系统错误。 */
        return;                     /* 不阻塞主流程。 */
    }
    usleep(200000);                 /* 给被杀进程一点退出和释放图层的时间。 */
    printf("lvgl demo   : cleaned\n"); /* 统一打印清理动作已经执行。 */
}

/* 计算从 start 到当前时刻经过的秒数。 */
static double elapsed_seconds_since(const struct timespec *start)
{
    struct timespec now;            /* 当前单调时钟。 */

    clock_gettime(CLOCK_MONOTONIC, &now); /* 获取当前时间。 */
    return (double)(now.tv_sec - start->tv_sec) +
           (double)(now.tv_nsec - start->tv_nsec) / 1000000000.0; /* 转成秒。 */
}

/* 初始化 FPS 统计窗口。 */
static void fps_meter_reset(struct fps_meter *meter)
{
    if (meter == NULL)              /* 防御空指针。 */
        return;                     /* 直接返回。 */
    clock_gettime(CLOCK_MONOTONIC, &meter->start); /* 记录窗口起点。 */
    meter->frames = 0;              /* 清空帧数。 */
}

/* 记录一帧；满 1 秒时刷新 UI 的 FPS 标签。 */
static void fps_meter_frame(struct fps_meter *meter)
{
    double seconds;                 /* 当前窗口经过秒数。 */
    uint32_t fps;                   /* 计算出的整数 FPS。 */

    if (meter == NULL)              /* 防御空指针。 */
        return;                     /* 直接返回。 */
    meter->frames++;                /* 累计一帧。 */
    seconds = elapsed_seconds_since(&meter->start); /* 计算窗口时间。 */
    if (seconds < 1.0)              /* 不足 1 秒不刷新，避免标签频繁跳动。 */
        return;                     /* 等下一个窗口。 */

    fps = (uint32_t)((double)meter->frames / seconds + 0.5); /* 四舍五入。 */
    t113_ui_lvgl_set_fps(fps);      /* 写到 UI 的 fps 标签。 */
    fps_meter_reset(meter);         /* 开启下一秒统计窗口。 */
}

/* 录制中的 LED 呼吸亮度：约 1.2 秒一个周期。 */
static uint8_t record_breath_brightness(void)
{
    struct timespec now;            /* 当前单调时钟。 */
    unsigned int phase;             /* 周期内位置，0~1199ms。 */

    clock_gettime(CLOCK_MONOTONIC, &now); /* 读取单调时钟。 */
    phase = (unsigned int)(((now.tv_sec * 1000ULL) +
                            (now.tv_nsec / 1000000ULL)) % 1200ULL);
    if (phase >= 600U)              /* 后半周期从亮变暗。 */
        phase = 1199U - phase;      /* 镜像成 0~599。 */
    return (uint8_t)(40U + (phase * 215U) / 599U); /* 亮度范围 40~255。 */
}

/* 实时预览：MJPG -> CedarC YUV -> G2D -> /dev/disp layer，直到 Ctrl+C。 */
static int run_preview(void)
{
    struct video_capture cap;        /* V4L2 摄像头采集上下文。 */
    struct disp_layer_display disp;  /* /dev/disp layer 显示上下文。 */
    struct g2d_display g2d;          /* G2D 显示上下文。 */
    struct cedar_decoder decoder;    /* CedarC 持久化解码器上下文。 */
    unsigned int frames = 0;         /* 成功显示的帧数。 */
    int ret = -1;                    /* 默认返回失败，正常结束时改为 0。 */

    memset(&cap, 0, sizeof(cap));    /* 清空摄像头上下文。 */
    memset(&disp, 0, sizeof(disp));  /* 清空 disp layer 上下文。 */
    memset(&g2d, 0, sizeof(g2d));    /* 清空 G2D 上下文。 */
    memset(&decoder, 0, sizeof(decoder)); /* 清空 CedarC 上下文。 */
    g2d.fd = -1;                    /* 标记 G2D 设备尚未打开。 */

    kill_lvgl_demo();               /* 停掉开机 demo，避免抢屏幕。 */

    if (disp_layer_display_open(&disp, 800, 480) < 0) /* 打开 /dev/disp layer。 */
        return -1;                  /* 打开失败直接返回。 */

    if (g2d_display_open(&g2d, DEFAULT_WIDTH, DEFAULT_HEIGHT) < 0) /* 打开 G2D。 */
        goto out_disp;              /* 失败则释放 disp layer。 */

    if (image_convert_open(&decoder, DEFAULT_WIDTH, DEFAULT_HEIGHT) < 0) /* 打开 CedarC。 */
        goto out_g2d;               /* 失败则释放 G2D。 */

    if (video_capture_open(&cap, DEFAULT_DEVICE, DEFAULT_WIDTH, DEFAULT_HEIGHT,
                           V4L2_PIX_FMT_MJPEG) < 0) /* 打开 UVC 摄像头。 */
        goto out_decoder;           /* 失败则释放 CedarC。 */

    if (video_capture_start(&cap) < 0) /* 启动 V4L2 stream。 */
        goto out_cap;               /* 失败则关闭摄像头。 */

    printf("preview     : press Ctrl+C to stop\n"); /* 提示退出方式。 */

    while (!g_stop) {               /* 未收到 Ctrl+C 时持续预览。 */
        struct video_frame frame;    /* V4L2 当前 MJPG 帧。 */
        struct yuv_image yuv;        /* CedarC 解码后的 YUV 帧。 */

        memset(&yuv, 0, sizeof(yuv));/* 清空 YUV 结构，失败时可安全释放。 */

        if (video_capture_get_frame(&cap, &frame) < 0) { /* 取一帧 MJPG。 */
            if (g_stop) {           /* Ctrl+C 可能打断 select。 */
                ret = 0;            /* 这种情况按正常退出处理。 */
                break;              /* 跳出循环走清理。 */
            }
            goto out_stop;          /* 采集失败则退出。 */
        }

        ret = image_convert_decode_mjpeg(&decoder, frame.data, frame.size,
                                         &yuv); /* CedarC 持久化硬解成 YUV。 */
        video_capture_put_frame(&cap, &frame); /* 及时归还 V4L2 缓冲。 */
        if (ret < 0)                /* 解码失败。 */
            goto out_stop;          /* 退出并清理资源。 */

        ret = g2d_display_stretch_yuv_to_dmabuf(&g2d, &yuv); /* G2D 输出到 dmabuf。 */
        image_convert_free_yuv(&yuv); /* 释放本帧 YUV 数据。 */
        if (ret < 0)                /* G2D 显示失败。 */
            goto out_stop;          /* 退出并清理资源。 */

        ret = disp_layer_display_show_argb8888(&disp,
                                               g2d_display_get_dst_fd(&g2d),
                                               g2d_display_get_dst_width(&g2d),
                                               g2d_display_get_dst_height(&g2d)); /* disp layer 显示。 */
        if (ret < 0)                /* disp 显示失败。 */
            goto out_stop;          /* 退出并清理资源。 */

        frames++;                   /* 统计成功显示帧数。 */
    }

    ret = 0;                        /* Ctrl+C 属于正常退出。 */

out_stop:
    video_capture_stop(&cap);       /* 停止 V4L2 stream。 */
out_cap:
    video_capture_close(&cap);      /* 关闭摄像头。 */
out_decoder:
    disp_layer_display_close(&disp);/* 先关闭 disp layer。 */
    g2d_display_close(&g2d);        /* 再释放 G2D。 */
    image_convert_close(&decoder);  /* 最后关闭 CedarC 持久化解码器。 */
    goto out_print_preview;
out_g2d:
    disp_layer_display_close(&disp);/* 确保 disp layer 先关闭。 */
    g2d_display_close(&g2d);        /* 关闭 G2D 并释放 ION。 */
    goto out_print_preview;
out_disp:
    disp_layer_display_close(&disp);/* 关闭 disp layer。 */

out_print_preview:
    printf("preview frames: %u\n", frames); /* 打印预览帧数。 */
    return ret;                     /* 返回执行结果。 */
}

/* 定时录制复用正式 UI 的音视频链路，避免 CLI 与主工程行为分叉。 */
static int run_record(int seconds)
{
    struct disp_layer_display ui_disp = {0};
    struct t113_ui_lvgl ui = {0};
    struct record_preview preview = {0};
    struct t113_ui_rect rect;
    uint32_t last_flush = 0;
    int64_t start;
    int ret = -1;

    preview.cap.fd = preview.g2d.fd = -1;
    kill_lvgl_demo();
    hide_video_overlay_layer();
    if (disp_layer_display_open_ex(&ui_disp, 800, 480, 1, 0, 10, 0, 0, 800, 480) < 0)
        return -1;
    if (t113_ui_lvgl_open(&ui, 800, 480) < 0)
        goto out_disp;
    t113_ui_lvgl_set_screen_title("Camera");
    if (!t113_ui_lvgl_get_video_rect(&rect) ||
        submit_ui_force(&ui_disp, &ui, &last_flush) < 0 ||
        record_preview_open(&preview, &rect) < 0 ||
        record_preview_step(&preview) < 0 || /* 先预热硬解，再启动麦克风，避免首帧初始化造成 overrun。 */
        record_preview_start_record(&preview, "video") < 0)
        goto out_preview;
    start = media_monotonic_ms();
    t113_ui_lvgl_set_playing(true);
    while (!g_stop && !storage_session_lost() && media_monotonic_ms() - start < seconds * 1000LL) { /* 定时录制按单调时间退出；拔盘和 Ctrl+C 同样触发收尾。 */
        if (record_preview_step(&preview) < 0) {
            if (g_stop)
                break;
            record_writer_abort(&preview.sink); /* 预览或显示异常使录制失败，不能将部分文件当完整录像。 */
            goto out_preview;
        }
        t113_ui_lvgl_set_rec_led(record_breath_brightness());
        if (submit_ui_if_dirty(&ui_disp, &ui, &last_flush) < 0) {
            record_writer_abort(&preview.sink); /* 预览或显示异常使录制失败，不能将部分文件当完整录像。 */
            goto out_preview;
        }
    }
    if (record_preview_stop_record(&preview) == 0) {
        printf("saved       : %s\nRESULT      : PASS\n", preview.record_path);
        ret = 0;
    }
out_preview:
    if (ret < 0 && preview.recording)
        record_writer_abort(&preview.sink); /* 预览或显示异常使录制失败，不能将部分文件当完整录像。 */
    record_preview_close(&preview);
    hide_video_overlay_layer();
    disp_layer_display_hide(&ui_disp);
    t113_ui_lvgl_close(&ui);
out_disp:
    disp_layer_display_close(&ui_disp);
    return ret;
}

/* 程序入口：无参数预览，一个 1~59 参数录制。 */
static int app_main(int argc, char **argv)
{
    int seconds;                    /* 录制秒数。 */
    int lock_fd;
    setvbuf(stdout, NULL, _IOLBF, 0); /* SSH 后台日志也逐行输出，便于排查。 */

    /* 同时启动两套显示/解码器会争用硬件；锁随进程退出自动释放。 */
    lock_fd = open("/tmp/t113_camera.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600); /* 进程锁避免多个实例同时控制显示图层和摄像头。 */
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0) { /* 非阻塞独占锁失败直接退出，不争用同一硬件。 */
        fprintf(stderr, "T113Camera already running or lock unavailable\n");
        if (lock_fd >= 0)
            close(lock_fd);
        return 1;
    }

    signal(SIGINT, on_signal);      /* 注册 Ctrl+C 信号处理。 */
    signal(SIGTERM, on_signal);     /* 注册终止信号处理。 */

    if (argc == 1)                  /* 无参数进入播放列表模式。 */
        return run_player_mode() == 0 ? 0 : 1; /* 按执行结果返回进程状态。 */

    if (argc == 2 && strcmp(argv[1], "preview") == 0) /* preview 参数进入实时预览。 */
        return run_preview() == 0 ? 0 : 1; /* 按执行结果返回进程状态。 */

    if (argc == 2 && strcmp(argv[1], "ui") == 0) /* ui 参数显示 LVGL 界面。 */
        return run_ui() == 0 ? 0 : 1; /* 按执行结果返回进程状态。 */

    if (argc == 3 && strcmp(argv[1], "ui") == 0) { /* ui + 文件路径播放媒体。 */
        if (path_ends_with_ci(argv[2], ".mkv") ||
            path_ends_with_ci(argv[2], ".avi") ||
            path_ends_with_ci(argv[2], ".mp4")) /* AVI/MKV/MP4 走 FFmpeg 解封装。 */
            return run_ui_mkv_video(argv[2]) == 0 ? 0 : 1;
        if (path_ends_with_ci(argv[2], ".wav")) /* WAV 走 ALSA 播放。 */
            return run_ui_wav(argv[2]) == 0 ? 0 : 1;
        if (path_ends_with_ci(argv[2], ".aac") ||
            path_ends_with_ci(argv[2], ".m4a") ||
            path_ends_with_ci(argv[2], ".mp3")) /* 压缩音频走 FFmpeg 解码播放。 */
            return run_ui_ffmpeg_audio(argv[2]) == 0 ? 0 : 1;
        return run_ui_video(argv[2]) == 0 ? 0 : 1; /* 其他暂按裸 MJPEG。 */
    }

    if (argc == 2) {                /* 一个参数表示录制秒数。 */
        if (parse_record_seconds(argv[1], &seconds) < 0) { /* 校验秒数。 */
            print_usage(argv[0]);    /* 参数非法时打印用法。 */
            return 1;               /* 返回失败。 */
        }
        return run_record(seconds) == 0 ? 0 : 1; /* 执行录制并返回状态。 */
    }

    print_usage(argv[0]);           /* 其他参数个数都非法。 */
    return 1;                       /* 返回失败。 */
}

/* 资源入口只管理存储线程；应用逻辑继续由 app_main 分派。 */
int main(int argc, char **argv)
{
    int ret, seconds; struct storage_snapshot storage; /* 退出码和启动挂载快照。 */
    g_storage_enabled = argc == 1 || (argc == 2 && parse_record_seconds(argv[1], &seconds) == 0) ||
                        (argc == 3 && !strncmp(argv[2], "/mnt/usb/", 9)); /* 非 U 盘独立显示不受热插拔影响。 */
    if (g_storage_enabled) { /* 启动一次，覆盖播放内部循环和录制后台写入。 */
        if (storage_monitor_start(&g_storage, NULL, NULL) < 0) return 1; /* 监控失败时拒绝运行存储任务。 */
        storage_monitor_snapshot(&g_storage, &storage); /* 取真实挂载路径，不假设 sda。 */
        snprintf(g_storage_root, sizeof(g_storage_root), "%s", storage.root); /* 无盘保持为空。 */
    }
    ret = app_main(argc, argv); /* 所有原命令返回路径集中经过资源收尾。 */
    if (g_storage_enabled) { /* 所有媒体线程已 join，取消标志不会再被读取。 */
        storage_bind_task(NULL); storage_monitor_close(&g_storage); /* 解绑后 join 监控，再销毁锁。 */
    }
    return ret; /* 保留原有命令成功/失败语义。 */
}
