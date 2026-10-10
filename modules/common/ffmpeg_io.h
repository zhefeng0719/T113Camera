#ifndef FFMPEG_IO_H
#define FFMPEG_IO_H
#include <stdint.h>
#include <time.h>
struct ffmpeg_io_guard {
    volatile int *stop; /* 借用会话取消标志，FFmpeg 调用结束前必须保持存活。 */
    int64_t deadline_us; /* 单次 I/O 尝试的单调时钟截止时间。 */
};
static inline int64_t ffmpeg_io_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}
static inline void ffmpeg_io_begin(struct ffmpeg_io_guard *guard)
{
    guard->deadline_us = ffmpeg_io_now() + 5000000; /* 每次读取前设置五秒期限，不是整个文件的播放时限。 */
}
static inline int ffmpeg_io_interrupt(void *opaque)
{
    struct ffmpeg_io_guard *guard = opaque; /* FFmpeg 回调恢复调用者持有的取消上下文。 */
    return (guard->stop && __atomic_load_n(guard->stop, __ATOMIC_ACQUIRE)) || /* 取消优先；与存储监控或控制线程 release 发布配对。 */
           ffmpeg_io_now() >= guard->deadline_us; /* 超时返回非零，请求 FFmpeg 退出；无法抢占阻塞的内核驱动。 */
}
#endif
