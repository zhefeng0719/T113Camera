#ifndef RECORD_WRITER_H
#define RECORD_WRITER_H
#include "audio_capture.h"
#include "ffmpeg_mjpeg_muxer.h"

#define RECORD_VIDEO_QUEUE 8 /* JPEG 写队列深度，不能因慢盘无限增长。 */
#define RECORD_MAX_JPEG (512U * 1024U) /* 单帧上限限制异常输入的堆分配。 */
struct record_video_block {
    unsigned char *data; /* JPEG 自有副本，提交成功后由写线程释放。 */
    size_t size; /* 压缩 JPEG 长度，不是解码像素大小。 */
    int64_t pts_us; /* 摄像头单调采集时间，保留排队前的时刻。 */
};
/* The writer is the only thread allowed to touch the muxer while running. */
struct record_writer {
    struct ffmpeg_mjpeg_muxer *muxer; /* 借用封装器；活动期只有写线程能写入。 */
    struct audio_queue *audio; /* 借用 PCM 队列，finish 前采集线程应已 join。 */
    struct record_video_block items[RECORD_VIDEO_QUEUE];
    unsigned int head, count; /* JPEG 环形队列读位置和占用数量，受 mutex 保护。 */
    int finish, failed, started; /* 正常排空请求、粘滞失败与线程启动状态。 */
    const volatile int *cancel; /* 可选会话取消信号；拥有者必须活到 finish/join 完成。 */
    pthread_mutex_t mutex; /* 保护 JPEG 队列，不持锁进行文件 I/O。 */
    pthread_t thread; /* 封装写线程，finish 负责 join。 */
};
int record_writer_start(struct record_writer *writer, struct ffmpeg_mjpeg_muxer *muxer,
                        struct audio_queue *audio);
int record_writer_start_cancelable(struct record_writer *writer, struct ffmpeg_mjpeg_muxer *muxer,
                                   struct audio_queue *audio, const volatile int *cancel); /* 不依赖 USB 检测实现。 */
int record_writer_submit(struct record_writer *writer, const void *data, size_t size, int64_t pts_us); /* 复制 JPEG 后入队；队列满时失败而不是悄悄丢录像帧。 */
void record_writer_abort(struct record_writer *writer); /* 只发布失败，不从外部线程关闭封装器。 */
/* Call only after the audio producer has joined: drains both queues before returning. */
int record_writer_finish(struct record_writer *writer);
#endif
