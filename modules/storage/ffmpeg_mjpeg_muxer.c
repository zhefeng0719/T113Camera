/*
 * ffmpeg_mjpeg_muxer.c
 *
 * 使用 FFmpeg libavformat 把摄像头输出的 MJPG 帧和 ALSA PCM 封装进 AVI。
 * 这里不做软件编码，解决的是视频、音频 packet 进入同一个容器的问题。
 */

#include "ffmpeg_mjpeg_muxer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/codec_id.h>
#include <libavutil/channel_layout.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>

/* 获取单调时钟毫秒，用来给每帧写入真实时间戳。 */
static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* 创建单级目录；已经存在也算成功。 */
static int make_dir(const char *path)
{
    if (mkdir(path, 0777) == 0)
        return 0;
    if (errno == EEXIST)
        return 0;
    perror(path);
    return -1;
}

/* 拼接 root/suffix 路径，并检查是否被截断。 */
static int build_path(char *out, size_t out_len,
                      const char *root, const char *suffix)
{
    int n;

    n = snprintf(out, out_len, "%s/%s", root, suffix);
    if (n < 0 || (size_t)n >= out_len) {
        fprintf(stderr, "path too long: %s/%s\n", root, suffix);
        return -1;
    }
    return 0;
}

/* 保证 U 盘中存在 T113Camera/VIDEO 目录。 */
static int ensure_video_dir(const char *usb_root)
{
    char path[512];

    if (build_path(path, sizeof(path), usb_root, "T113Camera") < 0)
        return -1;
    if (make_dir(path) < 0)
        return -1;

    if (build_path(path, sizeof(path), usb_root, "T113Camera/VIDEO") < 0)
        return -1;
    if (make_dir(path) < 0)
        return -1;

    return 0;
}

/* 打开指定名字的 AVI 输出文件，可选创建 PCM 音频流。 */
int ffmpeg_mjpeg_muxer_open_av_named(struct ffmpeg_mjpeg_muxer *muxer,
                                     const char *usb_root,
                                     const char *base_name,
                                     unsigned int width,
                                     unsigned int height,
                                     unsigned int fallback_fps,
                                     unsigned int audio_rate,
                                     unsigned int audio_channels,
                                     unsigned int audio_bits,
                                     char *out_path,
                                     size_t out_len)
{
    char suffix[128];
    int n;
    int ret;

    if (muxer == NULL || usb_root == NULL || width == 0 || height == 0)
        return -1;

    memset(muxer, 0, sizeof(*muxer));
    muxer->last_pts = -1;
    muxer->width = width;
    muxer->height = height;
    muxer->audio_rate = audio_rate;
    muxer->audio_channels = audio_channels;
    muxer->audio_bits = audio_bits;
    muxer->fallback_fps = fallback_fps != 0 ? fallback_fps : 30;
    muxer->frame_ms = 1000U / muxer->fallback_fps;
    if (muxer->frame_ms == 0)
        muxer->frame_ms = 1;

    if (ensure_video_dir(usb_root) < 0)
        return -1;
    if (base_name == NULL || base_name[0] == '\0')
        base_name = "video";

    n = snprintf(suffix, sizeof(suffix), "T113Camera/VIDEO/%s.avi", base_name);
    if (n < 0 || (size_t)n >= sizeof(suffix)) {
        fprintf(stderr, "record name too long: %s\n", base_name);
        return -1;
    }
    if (build_path(muxer->path, sizeof(muxer->path), usb_root, suffix) < 0)
        return -1;

    ret = avformat_alloc_output_context2(&muxer->fmt, NULL, "avi",
                                         muxer->path);
    if (ret < 0 || muxer->fmt == NULL) {
        fprintf(stderr, "avformat_alloc_output_context2 failed: %d\n", ret);
        return -1;
    }

    muxer->video_stream = avformat_new_stream(muxer->fmt, NULL);
    if (muxer->video_stream == NULL) {
        fprintf(stderr, "avformat_new_stream video failed\n");
        goto fail;
    }

    muxer->video_stream->time_base = (AVRational){1, 1000};
    muxer->video_stream->avg_frame_rate = (AVRational){(int)muxer->fallback_fps, 1};
    muxer->video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    muxer->video_stream->codecpar->codec_id = AV_CODEC_ID_MJPEG;
    muxer->video_stream->codecpar->width = (int)width;
    muxer->video_stream->codecpar->height = (int)height;
    muxer->video_stream->codecpar->format = AV_PIX_FMT_YUVJ422P;

    if (audio_rate > 0 && audio_channels > 0 && audio_bits == 16) {
        muxer->audio_stream = avformat_new_stream(muxer->fmt, NULL);
        if (muxer->audio_stream == NULL) {
            fprintf(stderr, "avformat_new_stream audio failed\n");
            goto fail;
        }
        muxer->audio_stream->time_base = (AVRational){1, (int)audio_rate};
        muxer->audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        muxer->audio_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        muxer->audio_stream->codecpar->format = AV_SAMPLE_FMT_S16;
        muxer->audio_stream->codecpar->sample_rate = (int)audio_rate;
        muxer->audio_stream->codecpar->channels = (int)audio_channels;
        muxer->audio_stream->codecpar->channel_layout =
            av_get_default_channel_layout((int)audio_channels);
        muxer->audio_stream->codecpar->bits_per_coded_sample = (int)audio_bits;
        muxer->audio_stream->codecpar->bits_per_raw_sample = (int)audio_bits;
        muxer->audio_stream->codecpar->block_align =
            (int)(audio_channels * audio_bits / 8U);
        muxer->audio_stream->codecpar->bit_rate =
            (int64_t)audio_rate * (int64_t)audio_channels * (int64_t)audio_bits;
    }

    ret = avio_open(&muxer->fmt->pb, muxer->path, AVIO_FLAG_WRITE);
    if (ret < 0) {
        fprintf(stderr, "avio_open failed: %s (%d)\n", muxer->path, ret);
        goto fail;
    }

    ret = avformat_write_header(muxer->fmt, NULL);
    if (ret < 0) {
        fprintf(stderr, "avformat_write_header failed: %d\n", ret);
        goto fail;
    }

    if (out_path != NULL && out_len > 0)
        snprintf(out_path, out_len, "%s", muxer->path);
    return 0;

fail:
    ffmpeg_mjpeg_muxer_close(muxer);
    return -1;
}

/* 打开指定名字的无音频 AVI 输出文件。 */
int ffmpeg_mjpeg_muxer_open_named(struct ffmpeg_mjpeg_muxer *muxer,
                                  const char *usb_root,
                                  const char *base_name,
                                  unsigned int width,
                                  unsigned int height,
                                  unsigned int fallback_fps,
                                  char *out_path,
                                  size_t out_len)
{
    return ffmpeg_mjpeg_muxer_open_av_named(muxer, usb_root, base_name,
                                            width, height, fallback_fps,
                                            0, 0, 0,
                                            out_path, out_len);
}

/* 打开默认 video.avi，兼容旧命令行录制入口。 */
int ffmpeg_mjpeg_muxer_open(struct ffmpeg_mjpeg_muxer *muxer,
                            const char *usb_root,
                            unsigned int width,
                            unsigned int height,
                            unsigned int fallback_fps,
                            char *out_path,
                            size_t out_len)
{
    return ffmpeg_mjpeg_muxer_open_named(muxer, usb_root, "video",
                                         width, height, fallback_fps,
                                         out_path, out_len);
}

/* 写入一帧 MJPG 数据，时间戳用真实写入时间计算。 */
int ffmpeg_mjpeg_muxer_write(struct ffmpeg_mjpeg_muxer *muxer,
                             const void *data,
                             size_t size)
{
    AVPacket pkt;
    uint64_t now_ms;
    int64_t pts;
    int ret;

    if (muxer == NULL || muxer->fmt == NULL || muxer->video_stream == NULL ||
        data == NULL || size == 0)
        return -1;

    now_ms = monotonic_ms();
    if (muxer->frames == 0) {
        muxer->start_ms = now_ms;
        pts = 0;
    } else {
        pts = (int64_t)(now_ms - muxer->start_ms);
        if (pts <= muxer->last_pts)
            pts = muxer->last_pts + (int64_t)muxer->frame_ms;
    }

    av_init_packet(&pkt);
    pkt.data = (uint8_t *)data;
    pkt.size = (int)size;
    pkt.stream_index = muxer->video_stream->index;
    pkt.pts = pts;
    pkt.dts = pts;
    pkt.duration = (int64_t)muxer->frame_ms;
    pkt.flags = AV_PKT_FLAG_KEY;

    ret = av_interleaved_write_frame(muxer->fmt, &pkt);
    if (ret < 0) {
        fprintf(stderr, "av_interleaved_write_frame failed: %d\n", ret);
        return -1;
    }

    muxer->frames++;
    muxer->bytes += size;
    muxer->last_pts = pts;
    return 0;
}

/* 写入一段 PCM S16_LE 音频，时间戳按连续 sample 计数递增。 */
int ffmpeg_mjpeg_muxer_write_audio(struct ffmpeg_mjpeg_muxer *muxer,
                                   const void *data,
                                   size_t size,
                                   unsigned int frames)
{
    AVPacket pkt;
    int ret;

    if (muxer == NULL || muxer->fmt == NULL || muxer->audio_stream == NULL ||
        data == NULL || size == 0 || frames == 0)
        return -1;

    av_init_packet(&pkt);
    pkt.data = (uint8_t *)data;
    pkt.size = (int)size;
    pkt.stream_index = muxer->audio_stream->index;
    pkt.pts = (int64_t)muxer->audio_frames;
    pkt.dts = (int64_t)muxer->audio_frames;
    pkt.duration = (int64_t)frames;
    pkt.flags = AV_PKT_FLAG_KEY;

    ret = av_interleaved_write_frame(muxer->fmt, &pkt);
    if (ret < 0) {
        fprintf(stderr, "av_interleaved_write_frame audio failed: %d\n", ret);
        return -1;
    }

    muxer->audio_blocks++;
    muxer->audio_frames += frames;
    muxer->audio_bytes += size;
    return 0;
}

/* 写尾、关闭文件并同步到 U 盘。 */
int ffmpeg_mjpeg_muxer_close(struct ffmpeg_mjpeg_muxer *muxer)
{
    int ret = 0;

    if (muxer == NULL)
        return 0;

    if (muxer->fmt != NULL) {
        if (muxer->fmt->pb != NULL) {
            if (muxer->frames > 0 && av_write_trailer(muxer->fmt) < 0)
                ret = -1;
            avio_closep(&muxer->fmt->pb);
        }
        avformat_free_context(muxer->fmt);
    }

    muxer->fmt = NULL;
    muxer->video_stream = NULL;
    muxer->audio_stream = NULL;
    sync();
    return ret;
}
