#include "ffmpeg_audio_player.h"

#include "audio_alsa.h"
#include "audio_timeline.h"
#include "ffmpeg_io.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define AUDIO_OUT_PERIOD 1024U

struct audio_output { /* 音频线程私有输出状态；其他线程只读取 media_clock。 */
    struct audio_pcm *pcm;
    struct media_clock *clock;
    volatile int *stop; /* 借用控制线程的停止标志，退出前不能失效。 */
    volatile int *paused; /* 借用暂停标志，以原子读写同步。 */
    uint64_t submitted; /* 累计提交到 ALSA 的帧数，包含暂停重放前的回退处理。 */
    int64_t base_ms; /* 媒体时间起点，以毫秒表示。 */
    size_t frame_bytes;
    snd_pcm_uframes_t history_frames; /* 历史环形缓冲大小与声卡缓冲一致，保留所有待播样本。 */
    uint8_t *history; /* 保存已提交 PCM 副本，供不可靠硬件 pause 的替代实现使用。 */
    void (*progress)(uint32_t, uint32_t, void *);
    uint32_t duration;
    void *user;
};

static int flag(volatile int *value) /* 读取跨线程控制标志，volatile 本身不提供同步保证。 */
{
    return value && __atomic_load_n(value, __ATOMIC_ACQUIRE); /* acquire 与控制线程的 release 发布配对。 */
}

/* ALSA delay 是尚未发声的帧数，不能把 writei 成功当成已经播放。 */
static snd_pcm_sframes_t update_audio_clock(struct audio_output *out)
{
    snd_pcm_sframes_t delay = 0;
    uint64_t played;
    int64_t position;
    snd_pcm_sframes_t available;
    if (snd_pcm_delay(out->pcm->handle, &delay) < 0 || delay < 0) /* 查询声卡尚未消费的帧数；负值不能计入播放位置。 */
        delay = 0;
    available = snd_pcm_avail_update(out->pcm->handle); /* 查询可写空闲帧数，用于校验驱动 delay。 */
    /* 本板驱动在 PREPARED/PAUSED 状态的 delay 可能为零，用环形缓冲占用校验。 */
    if (available >= 0 && (snd_pcm_uframes_t)available <= out->history_frames) { /* 仅合理的声卡空闲数量用于推算缓冲占用。 */
        snd_pcm_sframes_t queued = (snd_pcm_sframes_t)out->history_frames - available;
        if (queued > delay) /* 驱动 delay 低估时采用更保守的实际占用估计。 */
            delay = queued; /* 仍排队的样本不能计入已播放位置。 */
    }
    if ((uint64_t)delay > out->submitted) /* 限制 delay 上界，防止无符号减法下溢。 */
        delay = (snd_pcm_sframes_t)out->submitted; /* 未提交过的样本不可能处于待播队列。 */
    played = out->submitted - delay; /* 只有已提交减去仍排队的部分才是真正已播放。 */
    position = out->base_ms + (int64_t)(played * 1000 / out->pcm->rate); /* 将已消费样本数换算为媒体毫秒时钟。 */
    media_clock_publish(out->clock, position); /* 给视频调度发布主时钟，不跨线程共享 PCM 句柄。 */
    if (out->progress)
        out->progress(position > 0 ? (uint32_t)position : 0, out->duration, out->user); /* 回调报告硬件消费位置；由主线程更新显示。 */
    return delay;
}

static int output_samples(struct audio_output *out, const uint8_t *data, unsigned int frames);

/* 本板 codec 的硬件 pause 恢复会前跳指针，采用保存待播样本/drop/重放。 */
static int handle_pause(struct audio_output *out)
{
    snd_pcm_sframes_t pending;
    uint8_t *replay = NULL;
    unsigned int i;
    if (!flag(out->paused))
        return 0;
    pending = update_audio_clock(out); /* 暂停前快照仍在声卡中等待播放的样本数。 */
    printf("audio pause: replay pending=%ld submitted=%llu\n",
           (long)pending, (unsigned long long)out->submitted);
    {
        if ((snd_pcm_uframes_t)pending > out->history_frames) /* 历史空间不足不能假装恢复，避免读取已覆盖数据。 */
            return -1;
        if (pending > 0) {
            replay = malloc((size_t)pending * out->frame_bytes); /* 只保存待播尾部，不重放已经发声的数据。 */
            if (!replay)
                return -1;
            for (i = 0; i < (unsigned int)pending; i++) /* 按时间顺序整理环形历史中的待播样本。 */
                memcpy(replay + i * out->frame_bytes,
                       out->history + ((out->submitted - pending + i) % out->history_frames) * out->frame_bytes, /* 按累计帧序号从历史环形缓冲取出待播帧。 */
                       out->frame_bytes);
        }
        snd_pcm_drop(out->pcm->handle); /* 立即清空硬件待播队列，暂停或停止时不继续发声。 */
        out->submitted -= pending; /* 回退待播帧计数，恢复后重交这些样本不重复推进时间。 */
    }
    media_clock_pause(out->clock, 1); /* 冻结音视频共享时钟。 */
    while (flag(out->paused) && !flag(out->stop)) /* 等待恢复，但切换文件可以立即终止暂停。 */
        usleep(5000);
    if (flag(out->stop)) {
        free(replay); /* 重放已完成或停止时释放临时连续缓冲。 */
        return 1;
    }
    if (snd_pcm_prepare(out->pcm->handle) < 0) { /* drop 后重新准备 PCM，恢复失败不能继续提交声音。 */
        free(replay); /* 重放已完成或停止时释放临时连续缓冲。 */
        return -1;
    }
    media_clock_pause(out->clock, 0); /* 恢复共享时钟，再提交保留的样本。 */
    if (replay) {
        int ret = output_samples(out, replay, (unsigned int)pending); /* 暂停恢复重放待播尾部；仍遵守停止和分块提交逻辑。 */
        free(replay); /* 重放已完成或停止时释放临时连续缓冲。 */
        return ret;
    }
    return 0;
}

/* 非阻塞、分块提交，切换和暂停不必等待整段音频或 ALSA drain。 */
static int output_samples(struct audio_output *out, const uint8_t *data, unsigned int frames)
{
    unsigned int offset = 0;
    while (offset < frames && !flag(out->stop)) { /* 可在块间响应停止，避免一次阻塞写入大量样本。 */
        unsigned int chunk = frames - offset;
        snd_pcm_sframes_t wrote;
        unsigned int i;
        int ret = handle_pause(out);
        if (ret)
            return ret;
        if (chunk > AUDIO_OUT_PERIOD)
            chunk = AUDIO_OUT_PERIOD; /* 每轮最多一个 period，限制暂停和停止响应延迟。 */
        wrote = snd_pcm_writei(out->pcm->handle, data + offset * out->frame_bytes, chunk); /* 非阻塞写入交错 PCM；短写时只推进实际成功帧数。 */
        if (wrote == -EINTR)
            continue;
        if (wrote == -EAGAIN || wrote == 0) { /* 声卡暂时无空槽，刷新消费时钟后短暂等待。 */
            update_audio_clock(out); /* 刷新真实消费进度，而不是按成功 writei 次数估计。 */
            usleep(5000);
            continue;
        }
        if (wrote == -EPIPE) { /* 播放 underrun，需要 prepare 恢复。 */
            /* underrun 时先记录已经消费的样本，再重新准备设备。 */
            update_audio_clock(out); /* 刷新真实消费进度，而不是按成功 writei 次数估计。 */
            if (snd_pcm_prepare(out->pcm->handle) < 0)
                return -1;
            continue;
        }
        if (wrote < 0)
            return -1;
        for (i = 0; i < (unsigned int)wrote; i++) /* 只保存 ALSA 实际接受的样本，短写尾部尚未提交。 */
            memcpy(out->history + ((out->submitted + i) % out->history_frames) * out->frame_bytes, /* 按累计序号覆盖历史槽，保留可能仍在硬件中待播的数据。 */
                   data + (offset + i) * out->frame_bytes, out->frame_bytes);
        out->submitted += wrote; /* 累计成功提交帧数，供硬件消费进度计算。 */
        offset += wrote; /* 推进当前输出块内的位置，保留未写完尾部。 */
        update_audio_clock(out); /* 刷新真实消费进度，而不是按成功 writei 次数估计。 */
    }
    return flag(out->stop) ? 1 : 0;
}

static int64_t rescale_to_ms(int64_t value, AVRational time_base)
{
    if (value == AV_NOPTS_VALUE) /* 没有时间戳时返回未知，不能对哨兵值做时间换算。 */
        return -1;
    return av_rescale_q(value, time_base, (AVRational){1, 1000}); /* 流时间基转换为毫秒，不能把原始 PTS 直接当毫秒。 */
}

static int find_audio_stream(AVFormatContext *fmt)
{
    unsigned int i;

    for (i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters *par = fmt->streams[i]->codecpar; /* 每条流各自拥有编码参数，容器中视频与音频分开。 */

        if (par->codec_type == AVMEDIA_TYPE_AUDIO)
            return (int)i;
    }
    return -1;
}

int ffmpeg_audio_probe(const char *path, struct ffmpeg_audio_info *info)
{
    return ffmpeg_audio_probe_interruptible(path, info, NULL); /* 保留不依赖存储监控的原接口。 */
}

int ffmpeg_audio_probe_interruptible(const char *path, struct ffmpeg_audio_info *info,
                                      volatile int *stop)
{
    AVFormatContext *fmt = NULL;
    struct ffmpeg_io_guard io = {0};
    io.stop = stop; /* FFmpeg 回调只读取通用取消信号，不调用 USB 检测。 */
    int stream_index;
    AVCodecParameters *par;
    int ret = -1;

    if (path == NULL || info == NULL)
        return -1;
    memset(info, 0, sizeof(*info));
    info->duration_ms = -1; /* 探测前时长未知，流或容器提供后再填充。 */

    fmt = avformat_alloc_context(); /* 先分配解封装上下文，以便打开文件前安装中断回调。 */
    if (!fmt)
        return -1;
    fmt->interrupt_callback = (AVIOInterruptCB){ffmpeg_io_interrupt, &io}; /* 将会话取消和超时条件交给 FFmpeg；不能中断已卡住的内核调用。 */
    ffmpeg_io_begin(&io); /* 为下一次阻塞探测或读取刷新五秒截止时间。 */
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
        return -1;
    if (avformat_find_stream_info(fmt, NULL) < 0) /* 读取少量数据探测流格式和时长。 */
        goto out;

    stream_index = find_audio_stream(fmt); /* 选择第一条音频流，不把视频 packet 送入音频解码器。 */
    if (stream_index < 0)
        goto out;

    par = fmt->streams[stream_index]->codecpar; /* 取得选中音轨的参数，其他轨道不参与音频配置。 */
    info->sample_rate = (unsigned int)par->sample_rate; /* 返回音轨声明的样本频率。 */
    info->channels = (unsigned int)par->channels; /* 返回源音轨声道数。 */
    if (fmt->streams[stream_index]->duration != AV_NOPTS_VALUE)
        info->duration_ms = rescale_to_ms(fmt->streams[stream_index]->duration,
                                          fmt->streams[stream_index]->time_base);
    if (info->duration_ms < 0 && fmt->duration != AV_NOPTS_VALUE)
        info->duration_ms = fmt->duration / 1000; /* 容器 duration 以微秒表示，换成毫秒作为备用总时长。 */
    ret = (info->sample_rate > 0 && info->channels > 0) ? 0 : -1;

out:
    avformat_close_input(&fmt); /* 关闭输入文件与解封装器。 */
    return ret;
}

int ffmpeg_audio_play(const char *path,
                      const char *device,
                      volatile int *stop,
                      volatile int *paused,
                      void (*progress_cb)(uint32_t played_ms,
                                          uint32_t total_ms,
                                          void *user),
                      void *user)
{
    return ffmpeg_audio_play_clock(path, device, stop, paused, progress_cb, user, NULL);
}

int ffmpeg_audio_play_clock(const char *path, const char *device,
                            volatile int *stop, volatile int *paused,
                            void (*progress_cb)(uint32_t, uint32_t, void *),
                            void *user, struct media_clock *clock)
{
    AVFormatContext *fmt = NULL;
    struct ffmpeg_io_guard io = {.stop = stop};
    AVCodecContext *codec = NULL;
    const AVCodec *decoder;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    SwrContext *swr = NULL;
    struct audio_pcm pcm;
    int stream_index;
    int64_t out_layout;
    int64_t in_layout;
    int out_channels;
    int out_rate;
    uint8_t *out_buf = NULL;
    int out_buf_size = 0;
    int64_t duration_ms = -1;
    int ret = -1;
    int eof = 0;
    struct audio_output output = {0};
    snd_pcm_uframes_t buffer_frames, period_frames;

    memset(&pcm, 0, sizeof(pcm));
    fmt = avformat_alloc_context(); /* 先分配解封装上下文，以便打开文件前安装中断回调。 */
    if (!fmt)
        goto out;
    fmt->interrupt_callback = (AVIOInterruptCB){ffmpeg_io_interrupt, &io}; /* 将会话取消和超时条件交给 FFmpeg；不能中断已卡住的内核调用。 */
    ffmpeg_io_begin(&io); /* 为下一次阻塞探测或读取刷新五秒截止时间。 */

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "ffmpeg audio: open failed: %s\n", path);
        goto out;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) /* 读取少量数据探测流格式和时长。 */
        goto out;

    stream_index = find_audio_stream(fmt); /* 选择第一条音频流，不把视频 packet 送入音频解码器。 */
    if (stream_index < 0)
        goto out;

    decoder = avcodec_find_decoder(fmt->streams[stream_index]->codecpar->codec_id); /* 按容器声明的音频 codec 选择 FFmpeg 软件解码器。 */
    if (decoder == NULL) {
        fprintf(stderr, "ffmpeg audio: decoder missing\n");
        goto out;
    }

    codec = avcodec_alloc_context3(decoder); /* 解码器实例只由此音频线程持有。 */
    if (codec == NULL)
        goto out;
    if (avcodec_parameters_to_context(codec, fmt->streams[stream_index]->codecpar) < 0) /* 复制采样率、声道和 codec extradata 等流参数。 */
        goto out;
    if (avcodec_open2(codec, decoder, NULL) < 0) /* 初始化 MP3、AAC 或 PCM 解码器。 */
        goto out;

    out_channels = codec->channels > 0 ? codec->channels : 1; /* 优先使用源声道数，缺失时退回 mono。 */
    if (out_channels > 2)
        out_channels = 2; /* 超过双声道时下混为立体声以匹配板载输出。 */
    out_rate = codec->sample_rate > 0 ? codec->sample_rate : 48000; /* 优先保留源采样率，缺失时退回 48kHz。 */
    out_layout = av_get_default_channel_layout(out_channels); /* 确定重采样输出的声道布局。 */
    in_layout = codec->channel_layout ? (int64_t)codec->channel_layout :
                av_get_default_channel_layout(codec->channels);
    if (duration_ms < 0 && fmt->streams[stream_index]->duration != AV_NOPTS_VALUE)
        duration_ms = rescale_to_ms(fmt->streams[stream_index]->duration,
                                    fmt->streams[stream_index]->time_base);
    if (duration_ms < 0 && fmt->duration != AV_NOPTS_VALUE)
        duration_ms = fmt->duration / 1000;

    swr = swr_alloc_set_opts(NULL, /* 配置采样格式、声道布局及采样率转换，不是容器封装。 */
                             out_layout,
                             AV_SAMPLE_FMT_S16, /* 输出交错有符号 16 位 PCM，适配 ALSA S16_LE。 */
                             out_rate,
                             in_layout,
                             codec->sample_fmt, /* 重采样输入格式来自解码器，AAC 常见 planar 浮点。 */
                             codec->sample_rate, /* 重采样输入采样率使用解码器实际参数。 */
                             0,
                             NULL);
    if (swr == NULL || swr_init(swr) < 0) /* 初始化转换器，处理 AAC 常见的 planar 浮点样本。 */
        goto out;

    if (audio_playback_open(&pcm, device, (unsigned int)out_rate,
                            (unsigned int)out_channels,
                            AUDIO_OUT_PERIOD) < 0)
        goto out;
    if (pcm.rate != (unsigned int)out_rate) /* 实际硬件采样率不符会改变音调和速度，拒绝继续。 */
        goto out;
    if (snd_pcm_get_params(pcm.handle, &buffer_frames, &period_frames) < 0 || !buffer_frames)
        goto out;
    output.pcm = &pcm; /* 输出辅助逻辑借用本线程持有的 ALSA 设备。 */
    output.clock = clock; /* 向共享时钟发布位置，NULL 表示无需音视频同步。 */
    output.stop = stop; /* 块输出期间也响应切换或热插拔取消。 */
    output.paused = paused; /* 暂停状态在提交每块样本前检查。 */
    output.frame_bytes = (size_t)out_channels * 2; /* S16 交错样本每帧为声道数乘两字节。 */
    output.history_frames = buffer_frames; /* 历史环形空间覆盖全部硬件待播容量。 */
    output.history = calloc(buffer_frames, output.frame_bytes); /* 分配与声卡缓冲等大的 PCM 历史区。 */
    output.progress = progress_cb; /* 保存纯数据进度回调，不从此模块直接操作界面。 */
    output.duration = duration_ms > 0 ? (uint32_t)duration_ms : 1;
    output.user = user; /* 进度回调的借用上下文，线程结束前应保持有效。 */
    if (!output.history || snd_pcm_nonblock(pcm.handle, 1) < 0) /* 必须成功建立重放历史和非阻塞输出才能继续。 */
        goto out;

    packet = av_packet_alloc(); /* packet 存放容器解封装后的压缩音频数据。 */
    frame = av_frame_alloc(); /* frame 存放软件解码后的样本。 */
    if (packet == NULL || frame == NULL)
        goto out;

    printf("ffmpeg audio: %s %d Hz %d ch duration=%lldms\n",
           avcodec_get_name(codec->codec_id),
           out_rate,
           out_channels,
           (long long)duration_ms);

    while (!flag(stop)) { /* 解封装与解码每轮都检查停止，支持暂停中拔盘。 */
        int read_ret;

        if (handle_pause(&output) < 0)
            goto out;
        if (flag(stop))
            break;

        ffmpeg_io_begin(&io); /* 为下一次阻塞探测或读取刷新五秒截止时间。 */
        read_ret = av_read_frame(fmt, packet); /* 解封装读一包数据；不是直接读取 PCM。 */
        if (flag(stop))
            break;
        if (read_ret < 0 && read_ret != AVERROR_EOF) /* 非 EOF 读错误是失败，不能当自然播放结束。 */
            goto out;
        eof = read_ret == AVERROR_EOF; /* 仅正常 EOF 才进入解码器排尾，其他错误不能伪装成功。 */
        if (!eof && packet->stream_index != stream_index) { /* 跳过视频或其他音轨，不送给当前音频解码器。 */
            av_packet_unref(packet); /* 释放本包负载引用，保留 packet 对象供下一轮复用。 */
            continue;
        }

        if (avcodec_send_packet(codec, eof ? NULL : packet) < 0) { /* 正常送压缩包；EOF 送 NULL 排出解码器缓存。 */
            av_packet_unref(packet); /* 释放本包负载引用，保留 packet 对象供下一轮复用。 */
            goto out;
        }
        av_packet_unref(packet); /* 释放本包负载引用，保留 packet 对象供下一轮复用。 */

        while (!flag(stop)) { /* 解封装与解码每轮都检查停止，支持暂停中拔盘。 */
            int samples;
            int need;
            int converted;
            int64_t target = -1;
            unsigned int gap = 0, skip = 0;

            read_ret = avcodec_receive_frame(codec, frame); /* 一包可能产生多帧，需要循环取到 EAGAIN 或 EOF。 */
            if (read_ret == AVERROR(EAGAIN) || read_ret == AVERROR_EOF) /* 当前输入已排出或解码彻底结束，需要下一包或退出。 */
                break;
            if (read_ret < 0)
                goto out;
            if (frame->best_effort_timestamp != AV_NOPTS_VALUE) { /* 有时间戳时按媒体位置对齐输出样本，不按线程速度推进。 */
                int64_t origin = fmt->start_time == AV_NOPTS_VALUE ? 0 :
                    av_rescale_q(fmt->start_time, AV_TIME_BASE_Q, (AVRational){1, out_rate});
                target = av_rescale_q(frame->best_effort_timestamp, /* 将帧 PTS 换算为目标输出样本序号。 */
                                      fmt->streams[stream_index]->time_base,
                                      (AVRational){1, out_rate}) - origin -
                         av_rescale(swr_get_delay(swr, codec->sample_rate), out_rate, codec->sample_rate); /* 扣除重采样器内部延迟，对齐首个实际输出样本。 */
                if (target < 0)
                    target = 0;
            }

            samples = av_rescale_rnd(swr_get_delay(swr, codec->sample_rate) + /* 输出容量包含重采样器内部缓存和本帧样本，向上取整。 */
                                     frame->nb_samples,
                                     out_rate,
                                     codec->sample_rate, /* 重采样输入采样率使用解码器实际参数。 */
                                     AV_ROUND_UP);
            need = av_samples_get_buffer_size(NULL, out_channels, samples, /* 根据输出声道和采样格式计算缓冲大小。 */
                                              AV_SAMPLE_FMT_S16, 1);
            if (need > out_buf_size) {
                uint8_t *new_buf = realloc(out_buf, (size_t)need); /* 按实际需要扩容；失败不丢失旧缓冲指针。 */

                if (new_buf == NULL)
                    goto out;
                out_buf = new_buf; /* 扩容成功后才替换原指针，失败仍保留旧内存可清理。 */
                out_buf_size = need; /* 记录容量，不把转换输出样本数与字节容量混淆。 */
            }

            converted = swr_convert(swr, /* 将解码器样本转换为交错 S16 PCM。 */
                                    &out_buf,
                                    samples,
                                    (const uint8_t **)frame->extended_data,
                                    frame->nb_samples);
            if (converted < 0)
                goto out;
            if (converted > 0) {
                if (target >= 0 && audio_timeline_plan(target, output.submitted, out_rate, /* 按 PTS 规划补静音或跳重叠，避免声音时间线漂移。 */
                                                       converted, &gap, &skip) < 0) {
                    fprintf(stderr, "audio timestamp discontinuity exceeds 10 seconds\n");
                    goto out;
                }
                while (gap && !flag(stop)) { /* 时间戳有缺口时分块补静音，保持视频与音频时间线。 */
                    uint8_t silence[AUDIO_OUT_PERIOD * 4] = {0}; /* 最多双声道 S16 的一 period 静音缓冲。 */
                    unsigned int chunk = gap > AUDIO_OUT_PERIOD ? AUDIO_OUT_PERIOD : gap;
                    if (output_samples(&output, silence, chunk) < 0)
                        goto out;
                    gap -= chunk; /* 逐步消耗待补样本数，避免按大缺口分配大块内存。 */
                }
                if (output_samples(&output, out_buf + skip * output.frame_bytes, /* 跳过已经输出过的重叠首部，仅播放剩余完整帧。 */
                                    (unsigned int)converted - skip) < 0)
                    goto out;
            }

            av_frame_unref(frame); /* 处理结束释放样本引用，下一轮复用 AVFrame。 */
        }
        if (eof)
            break;
    }

    /* 解码器和重采样器都需要排尾，不能只 flush FFmpeg decoder。 */
    while (!flag(stop) && swr_get_delay(swr, out_rate) > 0) { /* 正常 EOF 排出重采样器尾部；切换时不强行播放旧声音。 */
        int converted = swr_convert(swr, &out_buf, out_buf_size / (int)output.frame_bytes,
                                     NULL, 0);
        if (converted < 0)
            goto out;
        if (converted == 0)
            break;
        if (output_samples(&output, out_buf, (unsigned int)converted) < 0)
            goto out;
    }
    /* 自然结束等尾音，用户切换则立即 drop；等待期间仍可暂停或停止。 */
    while (!flag(stop) && update_audio_clock(&output) > 0) { /* 非阻塞等硬件播完尾音，期间仍可响应暂停和停止。 */
        if (handle_pause(&output) < 0)
            goto out;
        usleep(5000);
    }
    ret = 0;

out:
    if (pcm.handle)
        snd_pcm_drop(pcm.handle);
    media_clock_finish(clock); /* 标记音频结束，让较长的视频尾部继续按单调时间调度。 */
    free(output.history); /* 释放暂停恢复所需的历史样本区。 */
    audio_pcm_close(&pcm); /* 统一关闭音频线程拥有的设备句柄。 */
    free(out_buf); /* 释放重采样输出缓冲。 */
    if (frame != NULL)
        av_frame_free(&frame); /* 释放解码后的样本对象。 */
    if (packet != NULL)
        av_packet_free(&packet); /* 释放解封装 packet 对象及残余引用。 */
    if (swr != NULL)
        swr_free(&swr); /* 释放采样格式转换器。 */
    if (codec != NULL)
        avcodec_free_context(&codec); /* 释放音频解码器。 */
    if (fmt != NULL)
        avformat_close_input(&fmt); /* 关闭输入文件与解封装器。 */
    return ret;
}
