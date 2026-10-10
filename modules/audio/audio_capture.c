/*
 * audio_capture.c
 *
 * T113Camera 音频采集模块。
 *
 * 这一层只负责把 ALSA capture 数据变成带时间戳的 PCM block。
 * 它不写 WAV，不编码 AAC，也不做容器封装。
 */

#include "audio_capture.h"
#include "audio_alsa.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 只恢复已实测的 MIC1/ADC1 增益，不改变耳机/外放自动路由。 */
static int configure_capture_gain(void) /* 通过 ALSA mixer 调整前级与 ADC 增益，不修改输出路由。 */
{
    snd_mixer_t *mixer = NULL; /* mixer 句柄与 PCM 数据句柄不同，只访问控制项。 */
    const char *names[] = {"MIC1 gain volume", "ADC1 volume"}; /* 两个实测有效的控制项：模拟前级增益和 ADC 音量。 */
    const char *vars[] = {"T113_MIC_GAIN", "T113_ADC_VOLUME"}; /* 允许从环境变量覆盖默认增益，便于板端调试。 */
    long values[] = {8, 120}; /* 低于早期最大增益，减少底噪和削顶风险。 */
    int ret = -1;
    if (snd_mixer_open(&mixer, 0) < 0) /* 先分配控制句柄；失败时没有需要释放的 mixer。 */
        return -1;
    if (snd_mixer_attach(mixer, "hw:audiocodec") < 0 || /* 绑定板载 codec 控制设备，随后注册简单控制项。 */
        snd_mixer_selem_register(mixer, NULL, NULL) < 0 || snd_mixer_load(mixer) < 0) /* 加载当前驱动暴露的 mixer 控件，不是假设寄存器地址。 */
        goto out;
    for (unsigned int i = 0; i < 2; i++) { /* 分别配置两个增益控制项。 */
        snd_mixer_selem_id_t *id;
        snd_mixer_elem_t *elem;
        long min, max;
        const char *text = getenv(vars[i]); /* 读取当前控制项对应的可选覆盖值。 */
        if (text) { /* 有环境覆盖值才解析，缺省保留实测增益。 */
            char *end;
            errno = 0; /* 清除旧错误，strtol 后才能可靠识别溢出。 */
            values[i] = strtol(text, &end, 10); /* 十进制解析并保留结束指针，禁止含无效尾部的值。 */
            if (errno || end == text || *end) /* 范围溢出、空输入或多余字符均拒绝。 */
                goto out;
        }
        snd_mixer_selem_id_alloca(&id); /* 在栈上构造控制项标识，无需单独释放。 */
        snd_mixer_selem_id_set_name(id, names[i]); /* 按驱动控制项名称查找，不能用界面上的声道名代替。 */
        elem = snd_mixer_find_selem(mixer, id); /* 取得实际控件；缺少控件意味着镜像配置不匹配。 */
        if (!elem) /* 镜像没有目标控制项时拒绝使用未知增益继续录音。 */
            goto out;
        if (snd_mixer_selem_has_capture_volume(elem)) { /* 优先使用控件的 capture 音量接口。 */
            snd_mixer_selem_get_capture_volume_range(elem, &min, &max); /* 驱动返回的是原始控制范围，不等同于百分比或 dB。 */
            if (values[i] < min || values[i] > max ||
                snd_mixer_selem_set_capture_volume_all(elem, values[i]) < 0)
                goto out;
        } else {
            snd_mixer_selem_get_playback_volume_range(elem, &min, &max); /* 部分 codec 增益以 playback 能力暴露，兼容这种控件。 */
            if (values[i] < min || values[i] > max ||
                snd_mixer_selem_set_playback_volume_all(elem, values[i]) < 0)
                goto out;
        }
    }
    printf("capture gain: MIC1=%ld ADC1=%ld\n", values[0], values[1]);
    ret = 0;
out:
    snd_mixer_close(mixer); /* 统一释放 mixer；不影响已打开的 PCM 数据流。 */
    return ret;
}
#include <time.h>

static size_t audio_frame_bytes(unsigned int channels) /* 一个 PCM 帧含所有声道同一时刻的样本。 */
{
    return channels * AUDIO_CAPTURE_BITS_PER_SAMPLE / 8; /* S16_LE 每声道两字节，mono 一帧为两字节。 */
}

static int64_t now_monotonic_us(void) /* 录音时间戳使用单调时间，不受日期校准影响。 */
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts); /* 取得与视频采集时钟对齐的单调时间。 */
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
}

int audio_queue_init(struct audio_queue *queue, unsigned int capacity) /* 创建有界 PCM 队列，限制写盘落后时的内存占用。 */
{
    memset(queue, 0, sizeof(*queue)); /* 建立或回收空队列状态，初始化过程不与使用线程并发。 */
    queue->items = calloc(capacity, sizeof(queue->items[0])); /* 分配音频块描述符；每块的样本内存另行分配。 */
    if (queue->items == NULL)
        return -1;
    queue->capacity = capacity; /* 保存环形槽位数，索引取模使用此上限。 */
    if (pthread_mutex_init(&queue->mutex, NULL) != 0) /* 先创建锁，再创建依赖该锁的条件变量。 */
        goto fail;
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) /* 消费者等待有数据；失败时回收已创建的互斥锁。 */
        goto fail_mutex;
    if (pthread_cond_init(&queue->not_full, NULL) != 0) /* 生产者等待有空槽；失败按初始化的逆序回收。 */
        goto fail_not_empty;
    return 0;

fail_not_empty:
    pthread_cond_destroy(&queue->not_empty); /* 使用该条件变量的线程均退出后回收同步资源。 */
fail_mutex:
    pthread_mutex_destroy(&queue->mutex); /* 其他线程不再访问此对象后销毁互斥锁。 */
fail:
    free(queue->items); /* 回收队列槽位数组，残留块应先逐个释放。 */
    memset(queue, 0, sizeof(*queue)); /* 建立或回收空队列状态，初始化过程不与使用线程并发。 */
    return -1;
}

void audio_queue_stop(struct audio_queue *queue) /* 置粘滞停止标志，同时唤醒生产者与消费者。 */
{
    pthread_mutex_lock(&queue->mutex); /* 进入临界区，保护共享队列或状态快照。 */
    queue->stopped = 1; /* 停止后 push 不再接管新块，但已排队数据仍可 pop。 */
    pthread_cond_broadcast(&queue->not_empty); /* 唤醒所有可能等待数据的消费者，让其检查停止状态。 */
    pthread_cond_broadcast(&queue->not_full); /* 唤醒等待空槽的采集线程，避免退出时 join 卡住。 */
    pthread_mutex_unlock(&queue->mutex); /* 离开临界区，后续 I/O 和耗时处理不持有此锁。 */
}

/* 停止标志由采集和 UI 线程共享，读取时也必须持有同一把锁。 */
int audio_queue_is_stopped(struct audio_queue *queue)
{
    int stopped;

    pthread_mutex_lock(&queue->mutex); /* 进入临界区，保护共享队列或状态快照。 */
    stopped = queue->stopped; /* 持锁读取停止标志，不用 volatile 代替互斥同步。 */
    pthread_mutex_unlock(&queue->mutex); /* 离开临界区，后续 I/O 和耗时处理不持有此锁。 */
    return stopped;
}

void audio_block_free(struct audio_block *block) /* 释放当前持有的 PCM 内存并清空描述符。 */
{
    if (block == NULL)
        return;
    free(block->data); /* 释放此音频块持有的样本内存。 */
    memset(block, 0, sizeof(*block)); /* 清除样本指针和元数据，防止重复释放。 */
}

void audio_queue_destroy(struct audio_queue *queue) /* 调用前必须停止并 join 使用此队列的线程。 */
{
    unsigned int i;

    if (queue == NULL || queue->items == NULL)
        return;
    for (i = 0; i < queue->count; i++) { /* 销毁前只遍历尚在队列中的有效块。 */
        unsigned int pos = (queue->head + i) % queue->capacity; /* 从读指针起遍历仍由队列持有的块。 */
        audio_block_free(&queue->items[pos]); /* 回收尚未被写线程消费的 PCM。 */
    }
    pthread_cond_destroy(&queue->not_full); /* 使用该条件变量的线程均退出后回收同步资源。 */
    pthread_cond_destroy(&queue->not_empty); /* 使用该条件变量的线程均退出后回收同步资源。 */
    pthread_mutex_destroy(&queue->mutex); /* 其他线程不再访问此对象后销毁互斥锁。 */
    free(queue->items); /* 回收队列槽位数组，残留块应先逐个释放。 */
    memset(queue, 0, sizeof(*queue)); /* 建立或回收空队列状态，初始化过程不与使用线程并发。 */
}

int audio_queue_push(struct audio_queue *queue, const struct audio_block *block) /* 成功时转移 block.data 所有权；失败时仍由调用者释放。 */
{
    pthread_mutex_lock(&queue->mutex); /* 进入临界区，保护共享队列或状态快照。 */
    while (!queue->stopped && queue->count == queue->capacity) /* 循环验证谓词，避免条件变量虚假唤醒造成队列溢出。 */
        pthread_cond_wait(&queue->not_full, &queue->mutex); /* 等待时释放互斥锁，醒来后重新持锁。 */
    if (queue->stopped) { /* 停止后的 push 失败，不转移调用者样本所有权。 */
        pthread_mutex_unlock(&queue->mutex); /* 离开临界区，后续 I/O 和耗时处理不持有此锁。 */
        return -1;
    }
    queue->items[queue->tail] = *block; /* 浅复制描述符，队列接管样本内存而不重复拷贝 PCM。 */
    queue->tail = (queue->tail + 1) % queue->capacity; /* 环形写指针移动到下一槽。 */
    queue->count++; /* 锁内更新排队块数。 */
    pthread_cond_signal(&queue->not_empty); /* 通知消费者至少有一块可取。 */
    pthread_mutex_unlock(&queue->mutex); /* 离开临界区，后续 I/O 和耗时处理不持有此锁。 */
    return 0;
}

int audio_queue_pop(struct audio_queue *queue, struct audio_block *block) /* 阻塞取块；返回 1 有数据、0 停止且已排空。 */
{
    pthread_mutex_lock(&queue->mutex); /* 进入临界区，保护共享队列或状态快照。 */
    while (!queue->stopped && queue->count == 0) /* 未停止且为空才等待，停止后允许排空已有块。 */
        pthread_cond_wait(&queue->not_empty, &queue->mutex); /* 等待生产者提交样本或停止广播。 */
    if (queue->count == 0) { /* 无可消费块，返回空而不读取未初始化槽位。 */
        pthread_mutex_unlock(&queue->mutex); /* 离开临界区，后续 I/O 和耗时处理不持有此锁。 */
        return 0;
    }
    *block = queue->items[queue->head]; /* 将样本内存所有权交给消费者。 */
    memset(&queue->items[queue->head], 0, sizeof(queue->items[queue->head])); /* 清除旧指针，避免销毁队列时二次释放。 */
    queue->head = (queue->head + 1) % queue->capacity; /* 环形读指针前进。 */
    queue->count--; /* 锁内减少有效块数。 */
    pthread_cond_signal(&queue->not_full); /* 通知生产者有新空槽。 */
    pthread_mutex_unlock(&queue->mutex); /* 离开临界区，后续 I/O 和耗时处理不持有此锁。 */
    return 1;
}

int audio_queue_try_pop(struct audio_queue *queue, struct audio_block *block) /* 非阻塞取块，便于写线程交替处理音频和视频。 */
{
    pthread_mutex_lock(&queue->mutex); /* 进入临界区，保护共享队列或状态快照。 */
    if (queue->count == 0) { /* 无可消费块，返回空而不读取未初始化槽位。 */
        pthread_mutex_unlock(&queue->mutex); /* 离开临界区，后续 I/O 和耗时处理不持有此锁。 */
        return 0;
    }
    *block = queue->items[queue->head]; /* 将样本内存所有权交给消费者。 */
    memset(&queue->items[queue->head], 0, sizeof(queue->items[queue->head])); /* 清除旧指针，避免销毁队列时二次释放。 */
    queue->head = (queue->head + 1) % queue->capacity; /* 环形读指针前进。 */
    queue->count--; /* 锁内减少有效块数。 */
    pthread_cond_signal(&queue->not_full); /* 通知生产者有新空槽。 */
    pthread_mutex_unlock(&queue->mutex); /* 离开临界区，后续 I/O 和耗时处理不持有此锁。 */
    return 1;
}

void *audio_capture_thread_main(void *arg) /* 独立采集线程只产出带采集时间的 PCM，不调用封装器。 */
{
    struct audio_capture_worker *worker = arg; /* 线程上下文由主线程持有，join 前必须保持存活。 */
    struct audio_pcm capture;
    const char *device = worker->device ? worker->device : AUDIO_CAPTURE_DEVICE;
    unsigned int rate = worker->rate ? worker->rate : AUDIO_CAPTURE_RATE;
    unsigned int channels = worker->channels ? worker->channels : AUDIO_CAPTURE_CHANNELS;
    unsigned int period = worker->period_frames ? worker->period_frames : AUDIO_CAPTURE_PERIOD_FRAMES;
    size_t frame_bytes = audio_frame_bytes(channels); /* 帧数转字节数必须乘声道数和样本字节宽度。 */
    size_t buffer_bytes = period * frame_bytes; /* 为一个 ALSA period 分配临时读缓冲。 */
    unsigned char *buffer = NULL;
    int64_t last_data_us = now_monotonic_us(); /* 无数据超时从启动时开始计量。 */

    worker->ret = -1; /* 采集默认失败，仅正常退出后发布成功结果。 */
    if (configure_capture_gain() < 0) { /* 控制项配置失败则停止队列，避免用未知增益采集。 */
        fprintf(stderr, "capture gain configuration failed\n");
        audio_queue_stop(worker->queue); /* 正常或异常退出均唤醒其他线程，不能让消费者永久等待。 */
        return NULL;
    }
    if (audio_capture_open(&capture, device, rate, channels, period) < 0) {
        audio_queue_stop(worker->queue); /* 正常或异常退出均唤醒其他线程，不能让消费者永久等待。 */
        return NULL;
    }

    buffer = malloc(buffer_bytes); /* 循环复用该缓冲；送入队列的块使用独立内存。 */
    if (capture.rate != rate || capture.channels != channels) /* 硬件协商结果必须与封装参数相同，防止录音加速或尖声。 */
        goto out_buffer; /* 实际硬件参数必须匹配封装流参数。 */
    if (buffer == NULL) {
        perror("malloc audio capture buffer");
        goto out_capture;
    }
    if (snd_pcm_nonblock(capture.handle, 1) < 0) /* 非阻塞读取配合短等待，让停止请求能够及时被观察。 */
        goto out_buffer;

    while (!audio_queue_is_stopped(worker->queue)) { /* 队列停止是录音线程退出的统一条件。 */
        struct audio_block block;
        snd_pcm_sframes_t got;
        size_t bytes;

        got = snd_pcm_readi(capture.handle, buffer, period); /* 返回 PCM 帧数，不是字节数，可能小于 period。 */
        if (got == -EAGAIN || got == 0) { /* 暂时无数据并非故障，等待后重试。 */
            /* 100ms 轮询允许停止及时响应；设备连续两秒无数据则报告故障。 */
            if (now_monotonic_us() - last_data_us > 2000000) /* 两秒连续无样本判为设备故障，停止录制。 */
                goto out_buffer;
            snd_pcm_wait(capture.handle, 100); /* 最多等 100ms，然后重新检查停止与无数据超时。 */
            continue;
        }
        if (got == -EPIPE) { /* 采集 overrun 表示硬件环形缓冲的数据已丢失。 */
            fprintf(stderr, "capture overrun: timeline will pad missing samples\n");
            if (snd_pcm_prepare(capture.handle) < 0) /* 恢复 ALSA 状态，时间戳缺口交由封装层补静音。 */
                goto out_buffer;
            continue;
        }
        if (got == -EINTR) /* 信号中断的读取不生成样本，重新检查停止条件后重试。 */
            continue;
        if (got < 0) /* 未单独恢复的 ALSA 错误进入统一清理出口。 */
            goto out_buffer;
        last_data_us = now_monotonic_us(); /* 有效采集刷新无数据超时基准。 */

        bytes = (size_t)got * frame_bytes; /* 仅复制实际采集到的完整帧。 */
        memset(&block, 0, sizeof(block)); /* 新块没有旧指针，只有成功分配后才具备数据所有权。 */
        block.data = malloc(bytes); /* 分配可跨线程持有的 PCM 块。 */
        if (block.data == NULL) {
            perror("malloc audio block");
            goto out_buffer;
        }
        memcpy(block.data, buffer, bytes); /* 队列持有独立副本，下轮读取不会覆盖此块。 */
        {
            snd_pcm_sframes_t available = snd_pcm_avail_update(capture.handle); /* 估计读完后硬件中尚未读取的帧数。 */
            block.pts_us = now_monotonic_us() - /* 从当前时刻回推此块首样本的采集时间。 */
                           ((int64_t)got + (available > 0 ? available : 0)) * 1000000 / capture.rate; /* 扣除本块和硬件积压样本时长，避免把读线程调度延迟当作采集时刻。 */
        }
        block.frames = (unsigned int)got; /* 记录完整 PCM 帧数，供封装层推进样本时间线。 */
        block.rate = capture.rate; /* 携带驱动实际确认的采样率。 */
        block.channels = capture.channels; /* 携带声道数，避免写入时误解交错布局。 */
        block.bytes = bytes; /* 保存有效数据字节数。 */

        if (audio_queue_push(worker->queue, &block) < 0) { /* 停止时入队失败，生产者仍拥有该块。 */
            audio_block_free(&block); /* 入队未成功，生产者负责释放此块。 */
            break;
        }

        worker->captured_blocks++; /* 只统计已经成功入队的块。 */
        if (worker->captured_frames == 0) /* 仅第一块记录时间线起点。 */
            worker->first_sample_us = block.pts_us; /* 首样本时刻用于与录像起点核对。 */
        worker->last_sample_us = block.pts_us + (int64_t)got * 1000000 / capture.rate; /* 记录最后一个块结束的时间，而非线程返回时间。 */
        worker->captured_frames += (unsigned int)got; /* 累计真实采样数量，用于时长和同步核查。 */
    }

    worker->ret = 0; /* 循环因正常队列停止退出，结果可在 join 后读取。 */

out_buffer:
    free(buffer); /* 释放当前任务的临时数据缓冲；队列拥有的块另行清理。 */
out_capture:
    audio_pcm_close(&capture); /* PCM 句柄由采集线程独占并自行关闭。 */
    audio_queue_stop(worker->queue); /* 正常或异常退出均唤醒其他线程，不能让消费者永久等待。 */
    return NULL;
}
