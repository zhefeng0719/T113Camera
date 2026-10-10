#include "record_writer.h" /* 写线程接口，包含封装器与 PCM 队列类型。 */
#include <stdlib.h> /* malloc/free 管理复制出来的 JPEG 帧。 */
#include <string.h> /* memset/memcpy 管理环形队列。 */
#include <unistd.h> /* 空队列时短暂让出 CPU。 */

void record_writer_abort(struct record_writer *writer) /* 只置错误标志，不跨线程关闭 FFmpeg。 */
{
    if (writer->started) /* 仅对已经拥有工作线程的实例发送取消。 */
        __atomic_store_n(&writer->failed, 1, __ATOMIC_RELEASE); /* 发布粘滞错误，后续排空不能把失败改成成功。 */
}

static void *write_thread(void *arg) /* 唯一操作录制封装器的工作线程。 */
{
    struct record_writer *writer = arg; /* 取得应用传入且保持存活的上下文。 */
    for (;;) { /* 正常停止排空队列；出错或拔盘立即中止。 */
        struct record_video_block video = {0}; /* 本轮取出的 JPEG 所有权转移到此变量。 */
        struct audio_block audio; /* 从 PCM 队列接收一块音频。 */
        int finish, audio_count = 0; /* 区分正常排空与错误退出，限制音频批量。 */
        if (writer->cancel && __atomic_load_n(writer->cancel, __ATOMIC_ACQUIRE)) /* 读取通用会话取消，不依赖存储检测模块。 */
            record_writer_abort(writer); /* 会话失效：由写线程中止封装，不跨线程关闭 FFmpeg。 */
        if (__atomic_load_n(&writer->failed, __ATOMIC_ACQUIRE)) /* 失败后不再消费或写入新数据。 */
            break;
        /* 音频和视频交替排空，任何一路都不能长期独占写线程。 */
        while (audio_count < 8 && audio_queue_try_pop(writer->audio, &audio) > 0) { /* 每轮最多八块 PCM，防止音频饿死视频。 */
            int ret = ffmpeg_mjpeg_muxer_write_audio_at(writer->muxer, audio.data,
                                                       audio.bytes, audio.frames, audio.pts_us); /* 字节数、帧数和采集时间一起交给封装器。 */
            audio_block_free(&audio); /* 消费完成释放此音频块，即使写入失败也释放。 */
            audio_count++; /* 记录本轮音频工作量。 */
            if (ret < 0) { /* 磁盘或封装错误必须停止并向应用反馈。 */
                record_writer_abort(writer); /* 设置失败状态，禁止发布完整文件。 */
                break;
            }
        }
        if (__atomic_load_n(&writer->failed, __ATOMIC_ACQUIRE)) /* 失败后不再消费或写入新数据。 */
            break;
        pthread_mutex_lock(&writer->mutex); /* 保护主线程和写线程共享的 JPEG 队列。 */
        if (writer->count) { /* 只在队列非空时取出一帧。 */
            video = writer->items[writer->head]; /* 复制描述符，接管堆内存。 */
            memset(&writer->items[writer->head], 0, sizeof(video)); /* 清除槽位，避免结束时重复 free。 */
            writer->head = (writer->head + 1) % RECORD_VIDEO_QUEUE; /* 有界环形队列向前移动读指针。 */
            writer->count--; /* 取出一帧后减少排队计数。 */
        }
        finish = writer->finish; /* 锁内读取主线程发出的正常结束请求。 */
        pthread_mutex_unlock(&writer->mutex); /* 文件 I/O 在锁外进行，不阻塞采集提交。 */
        if (video.data) { /* 本轮拿到视频帧才调用封装器。 */
            int ret = ffmpeg_mjpeg_muxer_write_at(writer->muxer, video.data, video.size, video.pts_us); /* 保留采集 PTS，排队延迟不会改变录像时间。 */
            free(video.data); /* 写线程已接管此入队副本；封装调用结束后释放。 */
            if (ret < 0) { /* 磁盘或封装错误必须停止并向应用反馈。 */
                record_writer_abort(writer); /* 设置失败状态，禁止发布完整文件。 */
                break;
            }
        } else if (finish && !audio_count) { /* 视频和音频均排空后正常退出。 */
            break;
        } else if (!audio_count) { /* 暂时没有数据，等待生产者。 */
            usleep(2000); /* 避免空队列忙轮询耗尽 CPU。 */
        }
    }
    if (__atomic_load_n(&writer->failed, __ATOMIC_ACQUIRE)) {
        ffmpeg_mjpeg_muxer_abort(writer->muxer); /* 仅写线程修改封装器失败状态。 */
        audio_queue_stop(writer->audio); /* 唤醒 PCM 生产者，停止继续采集。 */
    }
    return NULL; /* 主线程随后 join，资源不会提前释放。 */
}

int record_writer_start(struct record_writer *writer, struct ffmpeg_mjpeg_muxer *muxer,
                        struct audio_queue *audio)
{
    return record_writer_start_cancelable(writer, muxer, audio, NULL); /* 保持原有独立测试接口。 */
}

int record_writer_start_cancelable(struct record_writer *writer, struct ffmpeg_mjpeg_muxer *muxer,
                                   struct audio_queue *audio, const volatile int *cancel)
{
    memset(writer, 0, sizeof(*writer)); /* 每次开始录制重置本实例。 */
    writer->muxer = muxer; /* 保存已经打开的封装器。 */
    writer->audio = audio; /* 引用调用者拥有的 PCM 队列。 */
    writer->cancel = cancel; /* 只依赖通用取消标志，不认识 USB、LVGL 或按键。 */
    muxer->cancel = cancel; /* 写线程退出后，最终发布也检查相同会话。 */
    if (pthread_mutex_init(&writer->mutex, NULL) != 0) /* 互斥锁初始化失败不得启动线程。 */
        return -1;
    /* 创建线程之前发布 started，避免工作线程提前报错却无法设置失败状态。 */
    writer->started = 1; /* 先发布状态，工作线程可能比 pthread_create 返回更早运行。 */
    if (pthread_create(&writer->thread, NULL, write_thread, writer) != 0) { /* 创建真正负责磁盘写入的线程。 */
        writer->started = 0; /* 工作线程已不存在，禁止再次提交。 */
        pthread_mutex_destroy(&writer->mutex); /* 释放队列同步资源。 */
        return -1;
    }
    return 0;
}

int record_writer_submit(struct record_writer *writer, const void *data, size_t size, int64_t pts_us)
{
    struct record_video_block video; /* 准备交给写线程的帧描述符。 */
    if (!writer->started) /* 未启动时不访问互斥锁或队列。 */
        return -1;
    if (!data || !size || size > RECORD_MAX_JPEG ||
        __atomic_load_n(&writer->failed, __ATOMIC_ACQUIRE))
    {
        record_writer_abort(writer); /* 设置失败状态，禁止发布完整文件。 */
        return -1;
    }
    video.data = malloc(size); /* 复制缓存有明确大小上限。 */
    if (!video.data) { /* 内存不足同样让整次录像失败。 */
        record_writer_abort(writer); /* 设置失败状态，禁止发布完整文件。 */
        return -1;
    }
    memcpy(video.data, data, size); /* V4L2 原缓存尽快归还；写线程只持有这份副本。 */
    video.size = size; /* 保存 JPEG 有效字节数。 */
    video.pts_us = pts_us; /* 保存原采集时间，不以写盘时间计时。 */
    pthread_mutex_lock(&writer->mutex); /* 保护主线程和写线程共享的 JPEG 队列。 */
    if (writer->finish || writer->count == RECORD_VIDEO_QUEUE ||
        __atomic_load_n(&writer->failed, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&writer->mutex); /* 文件 I/O 在锁外进行，不阻塞采集提交。 */
        free(video.data); /* 未入队的内存由提交者释放。 */
        record_writer_abort(writer); /* 设置失败状态，禁止发布完整文件。 */
        return -1;
    }
    writer->items[(writer->head + writer->count) % RECORD_VIDEO_QUEUE] = video; /* 把 JPEG 副本所有权转交给环形队列。 */
    writer->count++; /* 增加排队帧数，不允许无界积压。 */
    pthread_mutex_unlock(&writer->mutex); /* 文件 I/O 在锁外进行，不阻塞采集提交。 */
    return 0;
}

int record_writer_finish(struct record_writer *writer)
{
    unsigned int i; /* 结束时遍历剩余槽位。 */
    int ret; /* 向应用返回录制是否失败。 */
    if (!writer->started) /* 未启动时不访问互斥锁或队列。 */
        return 0;
    pthread_mutex_lock(&writer->mutex); /* 保护主线程和写线程共享的 JPEG 队列。 */
    writer->finish = 1; /* 正常结束：请求工作线程排空剩余数据。 */
    pthread_mutex_unlock(&writer->mutex); /* 文件 I/O 在锁外进行，不阻塞采集提交。 */
    pthread_join(writer->thread, NULL); /* 等待磁盘线程结束；内核阻塞的 write 不能被强制取消。 */
    ret = __atomic_load_n(&writer->failed, __ATOMIC_ACQUIRE) ? -1 : 0; /* 失败状态优先于正常结束请求。 */
    for (i = 0; i < RECORD_VIDEO_QUEUE; i++) /* 错误退出可能留下尚未消费的帧。 */
        free(writer->items[i].data); /* 释放残留副本，空槽位 free(NULL) 安全。 */
    pthread_mutex_destroy(&writer->mutex); /* 释放队列同步资源。 */
    writer->started = 0; /* 工作线程已不存在，禁止再次提交。 */
    return ret; /* 调用者据此决定是否可以正常收尾发布。 */
}
