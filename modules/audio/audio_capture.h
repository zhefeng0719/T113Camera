#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define AUDIO_CAPTURE_DEVICE "hw:0,0" /* 实测板载 codec PCM 节点。 */
#define AUDIO_CAPTURE_RATE 48000 /* 采样率单位 Hz，AVI 声明必须与此一致。 */
#define AUDIO_CAPTURE_CHANNELS 1 /* mono：一帧只有一个声道样本，实测使用 ADC1。 */
#define AUDIO_CAPTURE_PERIOD_FRAMES 1024 /* 每次目标读取 1024 帧，48kHz 下约 21.3ms。 */
#define AUDIO_CAPTURE_BITS_PER_SAMPLE 16 /* 有符号小端 16 位，每声道样本两字节。 */

struct audio_block {
    int64_t pts_us; /* 此块首样本的单调采集时间，单位微秒。 */
    unsigned int frames; /* 完整 PCM 帧数，不是字节数。 */
    unsigned int rate; /* 实际采样率，或 worker 请求的采样率。 */
    unsigned int channels; /* 每个样本帧的声道数。 */
    unsigned char *data; /* 独立 PCM 内存；push 成功后队列接管，pop 后消费者接管。 */
    size_t bytes; /* 有效 PCM 字节数，等于 frames 乘 channels 乘二。 */
};

struct audio_queue {
    struct audio_block *items; /* 环形槽位仅存描述符，各块数据独立分配。 */
    unsigned int capacity; /* 队列最多持有的音频块数。 */
    unsigned int head; /* 下一消费槽位，受 mutex 保护。 */
    unsigned int tail; /* 下一写入槽位，受 mutex 保护。 */
    unsigned int count; /* 有效块数量，停止后可继续排空。 */
    int stopped; /* 停止标志，禁止接收新块并唤醒等待者。 */
    pthread_mutex_t mutex; /* 保护队列索引、数量、停止状态和槽位。 */
    pthread_cond_t not_empty; /* 消费者等待数据或停止。 */
    pthread_cond_t not_full; /* 生产者等待空槽或停止。 */
};

struct audio_capture_worker {
    struct audio_queue *queue; /* 采集目标队列，在线程 join 前不得销毁。 */
    const char *device; /* 借用设备名字字符串，NULL 使用默认节点。 */
    unsigned int rate; /* 实际采样率，或 worker 请求的采样率。 */
    unsigned int channels; /* 每个样本帧的声道数。 */
    unsigned int period_frames; /* 采集批次大小，以完整 PCM 帧计。 */
    unsigned int captured_blocks; /* 成功入队块数，线程结束后读取。 */
    unsigned int captured_frames; /* 成功入队总帧数，用于录音时长核查。 */
    int64_t first_sample_us;        /* 第一块首样本的估算单调时间。 */
    int64_t last_sample_us;         /* 最后一块末样本的估算单调时间。 */
    int ret; /* 线程退出结果；join 后由控制线程读取。 */
};

int audio_queue_init(struct audio_queue *queue, unsigned int capacity);
void audio_queue_stop(struct audio_queue *queue);
int audio_queue_is_stopped(struct audio_queue *queue);
void audio_queue_destroy(struct audio_queue *queue); /* 使用者线程全部停止后调用，释放未消费块。 */
int audio_queue_push(struct audio_queue *queue, const struct audio_block *block); /* 成功转移 data 所有权；失败由调用者释放。 */
int audio_queue_pop(struct audio_queue *queue, struct audio_block *block); /* 阻塞取块，1 有数据、0 停止且排空。 */
int audio_queue_try_pop(struct audio_queue *queue, struct audio_block *block); /* 非阻塞取块，允许写线程交替消费 JPEG 和 PCM。 */
void audio_block_free(struct audio_block *block);
void *audio_capture_thread_main(void *arg); /* arg 为 audio_capture_worker，不写 WAV 或 AVI。 */

#endif
