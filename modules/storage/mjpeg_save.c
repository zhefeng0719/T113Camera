/*
 * mjpeg_save.c
 *
 * T113Camera 的 MJPEG 裸流保存模块。
 *
 * 当前不做视频编码，UVC 摄像头输出的 MJPG 帧本身就是 JPEG 压缩数据。
 * 录制时把多帧 MJPG 顺序写入 video.mjpeg，后续主项目再考虑容器封装。
 */

#include "mjpeg_save.h"             /* mjpeg_writer 和函数声明。 */

#include <errno.h>                  /* errno / EEXIST。 */
#include <stdio.h>                  /* snprintf / fopen / fwrite / perror。 */
#include <stdlib.h>                 /* malloc / free。 */
#include <string.h>                 /* memset。 */
#include <sys/stat.h>               /* mkdir。 */
#include <unistd.h>                 /* sync。 */

#define MJPEG_BATCH_SIZE (1024U * 1024U) /* 1MB 批量写缓冲。 */

/* 创建单级目录；目录已经存在时也算成功。 */
static int make_dir(const char *path)
{
    if (mkdir(path, 0777) == 0)     /* mkdir 成功。 */
        return 0;                   /* 返回成功。 */
    if (errno == EEXIST)            /* 目录已经存在。 */
        return 0;                   /* 也返回成功。 */
    perror(path);                   /* 打印创建目录失败原因。 */
    return -1;                      /* 返回失败。 */
}

/* 拼接 root/suffix 路径，并检查输出缓冲区长度。 */
static int build_path(char *out, size_t out_len,
                      const char *root, const char *suffix)
{
    int n;                          /* snprintf 需要写入的字符数。 */

    n = snprintf(out, out_len, "%s/%s", root, suffix); /* 拼接路径。 */
    if (n < 0 || (size_t)n >= out_len) { /* 检查路径是否被截断。 */
        fprintf(stderr, "path too long: %s/%s\n", root, suffix);
        return -1;                  /* 返回失败。 */
    }
    return 0;                       /* 拼接成功。 */
}

/* 保证 U 盘中存在 T113Camera/VIDEO 目录。 */
static int ensure_video_dir(const char *usb_root)
{
    char path[512];                 /* 临时保存目录路径。 */

    if (build_path(path, sizeof(path), usb_root, "T113Camera") < 0) /* 拼接一级目录。 */
        return -1;                  /* 路径过长。 */
    if (make_dir(path) < 0)         /* 创建 T113Camera。 */
        return -1;                  /* 创建失败。 */

    if (build_path(path, sizeof(path), usb_root, "T113Camera/VIDEO") < 0) /* 拼接 VIDEO。 */
        return -1;                  /* 路径过长。 */
    if (make_dir(path) < 0)         /* 创建 VIDEO。 */
        return -1;                  /* 创建失败。 */

    return 0;                       /* 目录准备完成。 */
}

/* 打开 MJPEG 裸流文件，后续每帧追加写入。 */
int mjpeg_writer_open_named(struct mjpeg_writer *writer, const char *usb_root,
                            const char *base_name,
                            char *out_path, size_t out_len)
{
    char suffix[128];               /* T113Camera/VIDEO/xxx.mjpeg。 */
    int n;                          /* snprintf 需要写入的字符数。 */

    memset(writer, 0, sizeof(*writer)); /* 清空写入器状态。 */

    if (ensure_video_dir(usb_root) < 0) /* 确认 U 盘目录存在。 */
        return -1;                  /* 目录不可用。 */

    if (base_name == NULL || base_name[0] == '\0') /* 名字不能为空。 */
        base_name = "video";        /* 兜底用旧文件名。 */

    n = snprintf(suffix, sizeof(suffix), "T113Camera/VIDEO/%s.mjpeg",
                 base_name);        /* 拼接相对路径。 */
    if (n < 0 || (size_t)n >= sizeof(suffix)) { /* 检查是否被截断。 */
        fprintf(stderr, "record name too long: %s\n", base_name);
        return -1;                  /* 返回失败。 */
    }

    if (build_path(writer->path, sizeof(writer->path), usb_root,
                   suffix) < 0)     /* 生成输出路径。 */
        return -1;                  /* 路径过长。 */

    writer->fp = fopen(writer->path, "wb"); /* 以覆盖方式打开输出文件。 */
    if (!writer->fp) {              /* 打开失败。 */
        perror(writer->path);       /* 打印系统错误。 */
        return -1;                  /* 返回失败。 */
    }

    if (out_path && out_len > 0)    /* 如果调用者需要输出路径。 */
        snprintf(out_path, out_len, "%s", writer->path); /* 复制路径给调用者。 */

    writer->batch = malloc(MJPEG_BATCH_SIZE); /* 分配批量写缓冲。 */
    if (!writer->batch) {           /* 分配失败。 */
        perror("malloc mjpeg batch");
        fclose(writer->fp);
        writer->fp = NULL;
        return -1;
    }
    writer->batch_size = MJPEG_BATCH_SIZE; /* 记录缓冲容量。 */

    return 0;                       /* 打开成功。 */
}

/* 打开默认 video.mjpeg，兼容旧命令行录制接口。 */
int mjpeg_writer_open(struct mjpeg_writer *writer, const char *usb_root,
                      char *out_path, size_t out_len)
{
    return mjpeg_writer_open_named(writer, usb_root, "video",
                                   out_path, out_len);
}

/* 把批量写缓冲里的数据真正写入文件。 */
static int mjpeg_writer_flush_batch(struct mjpeg_writer *writer)
{
    if (writer->batch_used == 0)    /* 没有待写数据。 */
        return 0;                   /* 直接成功。 */
    if (fwrite(writer->batch, 1, writer->batch_used, writer->fp) != writer->batch_used) {
        perror("write mjpeg");
        return -1;
    }
    writer->batch_used = 0;         /* 写完后清空批量缓冲。 */
    return 0;
}

/* 写入一帧 MJPG 数据，并累计帧数和字节数。 */
int mjpeg_writer_write(struct mjpeg_writer *writer, const void *data, size_t size)
{
    if (!writer->fp)                /* 文件没有打开。 */
        return -1;                  /* 返回失败。 */

    if (size > writer->batch_size) { /* 极端大帧直接写，避免缓冲放不下。 */
        if (mjpeg_writer_flush_batch(writer) < 0)
            return -1;
        if (fwrite(data, 1, size, writer->fp) != size) {
            perror("write mjpeg");
            return -1;
        }
    } else {                        /* 正常帧先进入批量缓冲。 */
        if (writer->batch_used + size > writer->batch_size) {
            if (mjpeg_writer_flush_batch(writer) < 0)
                return -1;
        }
        memcpy(writer->batch + writer->batch_used, data, size);
        writer->batch_used += size;
    }

    writer->frames++;               /* 累计帧数。 */
    writer->bytes += size;          /* 累计字节数。 */
    return 0;                       /* 写入成功。 */
}

/* 关闭 MJPEG 文件，并 sync 到 U 盘。 */
int mjpeg_writer_close(struct mjpeg_writer *writer)
{
    int ret = 0;                    /* 关闭结果，默认成功。 */

    if (writer->fp) {               /* 如果文件已经打开。 */
        if (mjpeg_writer_flush_batch(writer) < 0) /* 关闭前写出最后一批。 */
            ret = -1;
        if (fclose(writer->fp) != 0) { /* 关闭文件。 */
            perror("close mjpeg");  /* 打印关闭错误。 */
            ret = -1;               /* 标记失败。 */
        }
        writer->fp = NULL;          /* 防止重复关闭。 */
    }
    free(writer->batch);            /* 释放批量写缓冲。 */
    writer->batch = NULL;
    writer->batch_size = 0;
    writer->batch_used = 0;

    sync();                         /* 刷新到 U 盘，方便拔盘查看。 */
    return ret;                     /* 返回关闭结果。 */
}
