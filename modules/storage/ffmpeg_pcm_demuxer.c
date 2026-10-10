#include "ffmpeg_pcm_demuxer.h"

#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>

#include <stdio.h>
#include <string.h>

static int64_t rescale_to_ms(int64_t value, AVRational time_base)
{
    if (value == AV_NOPTS_VALUE) /* 缺失时间戳不能参与普通 rescale，返回未知位置。 */
        return -1;
    return av_rescale_q(value, time_base, (AVRational){1, 1000}); /* 流 PTS 刻度换算为毫秒，不同流的 time_base 不能混用。 */
}

int ffmpeg_pcm_demuxer_open(struct ffmpeg_pcm_demuxer *demuxer,
                            const char *path)
{
    unsigned int i;

    if (demuxer == NULL || path == NULL)
        return -1;

    memset(demuxer, 0, sizeof(*demuxer)); /* 建立空资源状态；open/close 均不与读取线程并发。 */
    demuxer->audio_stream = -1; /* -1 表示尚未找到 PCM 音频流。 */
    demuxer->duration_ms = -1; /* 未知时长用负数标记，不能误当零时长。 */
    av_init_packet(&demuxer->packet); /* 初始化可复用 packet，负载引用在下次读取前释放。 */

    if (avformat_open_input(&demuxer->fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "ffmpeg audio: open input failed: %s\n", path);
        return -1;
    }
    if (avformat_find_stream_info(demuxer->fmt, NULL) < 0) {
        fprintf(stderr, "ffmpeg audio: find stream info failed\n");
        ffmpeg_pcm_demuxer_close(demuxer);
        return -1;
    }

    for (i = 0; i < demuxer->fmt->nb_streams; i++) {
        AVStream *stream = demuxer->fmt->streams[i]; /* 遍历容器中的各轨道，音频和视频参数各自独立。 */
        AVCodecParameters *par = stream->codecpar; /* 读取容器声明的 codec 参数，不在解封装层解码。 */

        if (par->codec_type != AVMEDIA_TYPE_AUDIO) /* PCM 简化入口跳过视频流。 */
            continue;
        if (par->codec_id != AV_CODEC_ID_PCM_S16LE) /* 不包含音频解码，仅直接取已是 S16_LE 的原始 PCM。 */
            continue;
        if (par->sample_rate <= 0 || par->channels <= 0)
            continue;

        demuxer->audio_stream = (int)i; /* 保存可直接送 ALSA 的 PCM 轨道索引。 */
        demuxer->time_base = stream->time_base; /* 保存所选流的时间基，用于后续 packet PTS 换算。 */
        demuxer->sample_rate = (unsigned int)par->sample_rate; /* PCM 采样率决定播放时钟速度。 */
        demuxer->channels = (unsigned int)par->channels; /* 每帧包含各声道同一时刻样本。 */
        demuxer->bits_per_sample = 16; /* 该简化接口仅接收 PCM_S16LE，不解码 MP3 或 AAC。 */
        if (stream->duration != AV_NOPTS_VALUE) /* 优先使用该轨道自身的时长与时间基。 */
            demuxer->duration_ms = rescale_to_ms(stream->duration,
                                                 stream->time_base);
        break;
    }

    if (demuxer->audio_stream < 0) {
        ffmpeg_pcm_demuxer_close(demuxer);
        return -1;
    }
    if (demuxer->duration_ms < 0 && demuxer->fmt->duration != AV_NOPTS_VALUE)
        demuxer->duration_ms = demuxer->fmt->duration / 1000; /* 流时长未知时用容器微秒总时长退回毫秒。 */

    printf("ffmpeg audio: stream=%d %u Hz %u ch duration=%lldms\n",
           demuxer->audio_stream,
           demuxer->sample_rate,
           demuxer->channels,
           (long long)demuxer->duration_ms);
    return 0;
}

int ffmpeg_pcm_demuxer_read(struct ffmpeg_pcm_demuxer *demuxer,
                            const unsigned char **data,
                            size_t *size,
                            int64_t *pts_ms)
{
    int ret;

    if (data != NULL)
        *data = NULL; /* 先清空输出，失败时调用者不能误用上轮数据。 */
    if (size != NULL)
        *size = 0; /* 失败或 EOF 不返回残余包长度。 */
    if (pts_ms != NULL)
        *pts_ms = -1; /* 没有有效 PTS 时由上层采用备用调度。 */
    if (demuxer == NULL || demuxer->fmt == NULL)
        return -1;

    if (demuxer->packet_valid) { /* 上一包借用负载仍有效时，先释放旧引用。 */
        av_packet_unref(&demuxer->packet); /* 释放上一包；之前返回的借用 data 指针到这里失效。 */
        demuxer->packet_valid = 0; /* 清除有效标记，避免重复 unref 同一负载。 */
    }

    while ((ret = av_read_frame(demuxer->fmt, &demuxer->packet)) >= 0) {
        AVPacket *pkt = &demuxer->packet;

        if (pkt->stream_index != demuxer->audio_stream) { /* 读取时跳过非选中 PCM 音轨的数据。 */
            av_packet_unref(pkt); /* 跳过无关流或已转移负载的包，避免引用泄漏。 */
            continue;
        }

        demuxer->packet_valid = 1; /* 保留负载供调用者消费，直到下次读取或关闭。 */
        if (data != NULL)
            *data = pkt->data; /* 返回借用指针，仅在下一次读取或关闭前有效。 */
        if (size != NULL)
            *size = (size_t)pkt->size; /* 返回压缩包有效字节数，不是解码图像大小。 */
        if (pts_ms != NULL)
            *pts_ms = rescale_to_ms(pkt->pts, demuxer->time_base);
        return 1;
    }

    return 0;
}

void ffmpeg_pcm_demuxer_close(struct ffmpeg_pcm_demuxer *demuxer)
{
    if (demuxer == NULL)
        return;

    if (demuxer->packet_valid) { /* 上一包借用负载仍有效时，先释放旧引用。 */
        av_packet_unref(&demuxer->packet); /* 释放上一包；之前返回的借用 data 指针到这里失效。 */
        demuxer->packet_valid = 0; /* 清除有效标记，避免重复 unref 同一负载。 */
    }
    if (demuxer->fmt != NULL)
        avformat_close_input(&demuxer->fmt); /* 关闭文件、流和解封装缓存。 */
    memset(demuxer, 0, sizeof(*demuxer)); /* 建立空资源状态；open/close 均不与读取线程并发。 */
    demuxer->audio_stream = -1; /* -1 表示尚未找到 PCM 音频流。 */
}
