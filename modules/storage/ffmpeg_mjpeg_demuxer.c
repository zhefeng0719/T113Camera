/*
 * ffmpeg_mjpeg_demuxer.c
 *
 * 从 MKV/AVI/MP4 等容器中取出压缩视频帧。
 * 注意：这里不解码，只把 packet 交给 CedarC，避免走 FFmpeg 软件解码。
 */

#include "ffmpeg_mjpeg_demuxer.h"

#include <stdio.h>
#include <string.h>

static const char *codec_name(enum ffmpeg_video_codec codec)
{
    switch (codec) {
    case FFMPEG_VIDEO_CODEC_MJPEG:
        return "MJPEG";
    case FFMPEG_VIDEO_CODEC_H264:
        return "H264";
    case FFMPEG_VIDEO_CODEC_H265:
        return "H265";
    default:
        return "UNKNOWN";
    }
}

static int64_t rescale_to_ms(int64_t value, AVRational time_base)
{
    if (value == AV_NOPTS_VALUE) /* 缺失时间戳不能参与普通 rescale，返回未知位置。 */
        return -1;
    return av_rescale_q(value, time_base, (AVRational){1, 1000}); /* 流 PTS 刻度换算为毫秒，不同流的 time_base 不能混用。 */
}

int ffmpeg_mjpeg_demuxer_open(struct ffmpeg_mjpeg_demuxer *demuxer,
                              const char *path)
{
    return ffmpeg_mjpeg_demuxer_open_interruptible(demuxer, path, NULL);
}

int ffmpeg_mjpeg_demuxer_open_interruptible(struct ffmpeg_mjpeg_demuxer *demuxer,
                                            const char *path, volatile int *stop)
{
    unsigned int i;

    if (demuxer == NULL || path == NULL)
        return -1;

    memset(demuxer, 0, sizeof(*demuxer)); /* 建立空资源状态；open/close 均不与读取线程并发。 */
    demuxer->video_stream = -1; /* -1 表示尚未找到支持的视频流。 */
    demuxer->duration_ms = -1; /* 未知时长用负数标记，不能误当零时长。 */
    av_init_packet(&demuxer->packet); /* 初始化可复用 packet，负载引用在下次读取前释放。 */
    demuxer->io.stop = stop; /* 借用会话取消标志，关闭解封装器前必须保持有效。 */
    demuxer->fmt = avformat_alloc_context();
    if (!demuxer->fmt)
        return -1;
    demuxer->fmt->interrupt_callback = (AVIOInterruptCB){ffmpeg_io_interrupt, &demuxer->io};
    ffmpeg_io_begin(&demuxer->io); /* 每次探测或读包前刷新 I/O 超时期限。 */

    if (avformat_open_input(&demuxer->fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "ffmpeg: open input failed: %s\n", path);
        return -1;
    }

    if (avformat_find_stream_info(demuxer->fmt, NULL) < 0) {
        fprintf(stderr, "ffmpeg: find stream info failed\n");
        ffmpeg_mjpeg_demuxer_close(demuxer);
        return -1;
    }

    for (i = 0; i < demuxer->fmt->nb_streams; i++) {
        AVStream *stream = demuxer->fmt->streams[i]; /* 遍历容器中的各轨道，音频和视频参数各自独立。 */
        AVCodecParameters *par = stream->codecpar; /* 读取容器声明的 codec 参数，不在解封装层解码。 */

        if (par->codec_type != AVMEDIA_TYPE_VIDEO) /* 视频解封装入口跳过音频和其他轨道。 */
            continue;
        if (par->codec_id == AV_CODEC_ID_MJPEG)
            demuxer->codec = FFMPEG_VIDEO_CODEC_MJPEG;
        else if (par->codec_id == AV_CODEC_ID_H264)
            demuxer->codec = FFMPEG_VIDEO_CODEC_H264;
        else if (par->codec_id == AV_CODEC_ID_HEVC)
            demuxer->codec = FFMPEG_VIDEO_CODEC_H265;
        else
            continue;

        demuxer->video_stream = (int)i; /* 记录第一条支持的视频流索引。 */
        demuxer->time_base = stream->time_base; /* 保存所选流的时间基，用于后续 packet PTS 换算。 */
        demuxer->width = (unsigned int)par->width; /* 声明宽度用于初始化 CedarC；输出还需参考解码器真实几何。 */
        demuxer->height = (unsigned int)par->height; /* 声明高度不同于对齐后 YUV 缓冲高度。 */
        if (stream->duration != AV_NOPTS_VALUE) /* 优先使用该轨道自身的时长与时间基。 */
            demuxer->duration_ms = rescale_to_ms(stream->duration,
                                                 stream->time_base);
        break;
    }

    if (demuxer->video_stream < 0) {
        fprintf(stderr, "ffmpeg: no supported video stream found\n");
        ffmpeg_mjpeg_demuxer_close(demuxer);
        return -1;
    }

    if (demuxer->codec == FFMPEG_VIDEO_CODEC_H264 ||
        demuxer->codec == FFMPEG_VIDEO_CODEC_H265) {
        const AVBitStreamFilter *filter;
        AVStream *stream = demuxer->fmt->streams[demuxer->video_stream];

        filter = av_bsf_get_by_name(demuxer->codec == FFMPEG_VIDEO_CODEC_H265 ? /* 按 codec 选择 MP4 NAL 到 Annex-B 的格式转换。 */
                                    "hevc_mp4toannexb" : "h264_mp4toannexb"); /* 把 MP4 长度前缀 NAL 改为 CedarC 接受的 Annex-B 起始码。 */
        if (filter == NULL) {
            fprintf(stderr, "ffmpeg: annexb bitstream filter is missing\n");
            ffmpeg_mjpeg_demuxer_close(demuxer);
            return -1;
        }
        if (av_bsf_alloc(filter, &demuxer->bsf) < 0 ||
            avcodec_parameters_copy(demuxer->bsf->par_in, stream->codecpar) < 0 || /* 复制 extradata，使过滤器能带入 SPS/PPS 等必要配置。 */
            av_bsf_init(demuxer->bsf) < 0) { /* 初始化比特流格式转换，不是重编码。 */
            fprintf(stderr, "ffmpeg: annexb bitstream filter init failed\n");
            ffmpeg_mjpeg_demuxer_close(demuxer);
            return -1;
        }
        av_init_packet(&demuxer->filtered_packet); /* 初始化 FFmpeg packet；负载引用在使用结束后单独释放。 */
    }

    if (demuxer->duration_ms < 0 && demuxer->fmt->duration != AV_NOPTS_VALUE)
        demuxer->duration_ms = demuxer->fmt->duration / 1000; /* 流时长未知时用容器微秒总时长退回毫秒。 */

    printf("ffmpeg demux: stream=%d codec=%s %ux%u duration=%lldms\n",
           demuxer->video_stream,
           codec_name(demuxer->codec),
           demuxer->width,
           demuxer->height,
           (long long)demuxer->duration_ms);
    return 0;
}

int ffmpeg_mjpeg_demuxer_read(struct ffmpeg_mjpeg_demuxer *demuxer,
                              const unsigned char **data,
                              size_t *size,
                              int64_t *pts_ms,
                              int64_t *duration_ms)
{
    int ret;

    if (data != NULL)
        *data = NULL; /* 先清空输出，失败时调用者不能误用上轮数据。 */
    if (size != NULL)
        *size = 0; /* 失败或 EOF 不返回残余包长度。 */
    if (pts_ms != NULL)
        *pts_ms = -1; /* 没有有效 PTS 时由上层采用备用调度。 */
    if (duration_ms != NULL)
        *duration_ms = -1;
    if (demuxer == NULL || demuxer->fmt == NULL)
        return -1;

    if (demuxer->filtered_packet_valid) {
        av_packet_unref(&demuxer->filtered_packet); /* 释放上次 Annex-B 输出，调用者必须在下次 read 前消费或引用它。 */
        demuxer->filtered_packet_valid = 0; /* Annex-B 旧包引用已释放，后续输出重新标记有效。 */
    }
    if (demuxer->packet_valid) { /* 上一包借用负载仍有效时，先释放旧引用。 */
        av_packet_unref(&demuxer->packet); /* 释放上一包；之前返回的借用 data 指针到这里失效。 */
        demuxer->packet_valid = 0; /* 清除有效标记，避免重复 unref 同一负载。 */
    }

    for (;;) {
        ffmpeg_io_begin(&demuxer->io); /* 每次探测或读包前刷新 I/O 超时期限。 */
        ret = av_read_frame(demuxer->fmt, &demuxer->packet); /* 解封装获取压缩包，视频硬解仍由 CedarC 完成。 */
        if (ret < 0)
            break;
        AVPacket *pkt = &demuxer->packet;

        if (pkt->stream_index != demuxer->video_stream) { /* 音频与其他包不交给 CedarC 视频解码器。 */
            av_packet_unref(pkt); /* 跳过无关流或已转移负载的包，避免引用泄漏。 */
            continue;
        }

        if (demuxer->bsf != NULL) {
            ret = av_bsf_send_packet(demuxer->bsf, pkt); /* 向 Annex-B 过滤器提交压缩数据。 */
            av_packet_unref(pkt); /* 跳过无关流或已转移负载的包，避免引用泄漏。 */
            if (ret < 0)
                return -1;
            ret = av_bsf_receive_packet(demuxer->bsf, &demuxer->filtered_packet); /* 取得可交给硬解码器的比特流。 */
            if (ret == AVERROR(EAGAIN))
                continue;
            if (ret < 0)
                return -1;

            demuxer->filtered_packet_valid = 1; /* 过滤后包负载在下次 read 前保持有效。 */
            pkt = &demuxer->filtered_packet; /* 后续返回 Annex-B 负载而非原始 MP4 长度前缀数据。 */
        } else {
            demuxer->packet_valid = 1; /* 保留负载供调用者消费，直到下次读取或关闭。 */
        }

        if (data != NULL)
            *data = pkt->data; /* 返回借用指针，仅在下一次读取或关闭前有效。 */
        if (size != NULL)
            *size = (size_t)pkt->size; /* 返回压缩包有效字节数，不是解码图像大小。 */
        if (pts_ms != NULL)
            *pts_ms = pkt->pts == AV_NOPTS_VALUE ? -1 :
                      rescale_to_ms(pkt->pts, demuxer->time_base) -
                      (demuxer->fmt->start_time == AV_NOPTS_VALUE ? 0 : demuxer->fmt->start_time / 1000);
        if (duration_ms != NULL)
            *duration_ms = rescale_to_ms(pkt->duration, demuxer->time_base); /* 将包持续时间转换为视频调度使用的毫秒。 */
        return 1;
    }

    return (ret == AVERROR_EOF) ? 0 : -1; /* 区分正常读完与 I/O 错误，拔盘不能伪装 EOF。 */
}

void ffmpeg_mjpeg_demuxer_close(struct ffmpeg_mjpeg_demuxer *demuxer)
{
    if (demuxer == NULL)
        return;

    if (demuxer->filtered_packet_valid) {
        av_packet_unref(&demuxer->filtered_packet); /* 释放上次 Annex-B 输出，调用者必须在下次 read 前消费或引用它。 */
        demuxer->filtered_packet_valid = 0; /* Annex-B 旧包引用已释放，后续输出重新标记有效。 */
    }
    if (demuxer->packet_valid) { /* 上一包借用负载仍有效时，先释放旧引用。 */
        av_packet_unref(&demuxer->packet); /* 释放上一包；之前返回的借用 data 指针到这里失效。 */
        demuxer->packet_valid = 0; /* 清除有效标记，避免重复 unref 同一负载。 */
    }
    if (demuxer->bsf != NULL)
        av_bsf_free(&demuxer->bsf); /* 释放比特流过滤器及内部引用。 */
    if (demuxer->fmt != NULL)
        avformat_close_input(&demuxer->fmt); /* 关闭文件、流和解封装缓存。 */
    memset(demuxer, 0, sizeof(*demuxer)); /* 建立空资源状态；open/close 均不与读取线程并发。 */
    demuxer->video_stream = -1; /* -1 表示尚未找到支持的视频流。 */
}
