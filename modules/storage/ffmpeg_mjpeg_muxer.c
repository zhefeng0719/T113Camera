/*
 * ffmpeg_mjpeg_muxer.c
 *
 * 使用 FFmpeg libavformat 把摄像头输出的 MJPG 帧和 ALSA PCM 封装进 AVI。
 * 这里不做软件编码，解决的是视频、音频 packet 进入同一个容器的问题。
 */

#include "ffmpeg_mjpeg_muxer.h"
#include "atomic_publish.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/codec_id.h>
#include <libavutil/channel_layout.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mem.h>

/* 确认保存根目录就是挂载点，不能在 U 盘缺席时创建板载目录。 */
static int mounted_root(const char *root)
{
    char resolved[PATH_MAX];
    struct mntent *entry;
    struct mntent record; char buffer[4096]; /* 监控和封装可能同时解析挂载记录，必须使用线程独立缓冲。 */
    FILE *mounts;
    int found = 0;

    if (realpath(root, resolved) == NULL)
        return 0;
    mounts = setmntent("/proc/mounts", "r");
    if (mounts == NULL)
        return 0;
    while ((entry = getmntent_r(mounts, &record, buffer, sizeof(buffer))) != NULL) {
        if (strcmp(entry->mnt_dir, resolved) == 0) {
            found = 1;
            break;
        }
    }
    endmntent(mounts); /* 关闭挂载记录文件，快照不长期占用句柄。 */
    return found;
}

/* AVI 还会为时间戳间隙建立空帧索引，按时间槽预留收尾空间。 */
static int room_for_packet(struct ffmpeg_mjpeg_muxer *muxer, size_t size)
{
    struct statvfs space;
    uint64_t available;
    uint64_t video_slots = muxer->frames;
    uint64_t reserve;

    if (fstatvfs(muxer->file_fd, &space) < 0) { /* 按已打开文件所属文件系统查询，拔插不会换到另一个根路径。 */
        perror("record space check");
        muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
        return -1;
    }
    available = (uint64_t)space.f_bavail * space.f_frsize; /* 使用普通用户可用块数乘块大小，得到当前可写字节数。 */
    /* 当前视频时间基为毫秒，每个时间槽按 32 字节索引余量估算。 */
    if (muxer->last_pts >= 0 && (uint64_t)muxer->last_pts + 1 > video_slots)
        video_slots = (uint64_t)muxer->last_pts + 1; /* 毫秒时间基可能产生空帧槽，索引余量不能只按实际帧数估计。 */
    reserve = 8ULL * 1024ULL * 1024ULL + /* 基础预留八 MiB，再加入随录像增长的 AVI 索引预算。 */
              (video_slots + muxer->audio_blocks + 1ULL) * 32ULL; /* 对视频时间槽和音频块保守预留索引空间。 */
    if (available < reserve + size) { /* 剩余空间不够写当前包并完整收尾时提前失败。 */
        fprintf(stderr, "record stopped: insufficient free space\n");
        muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
        return -1;
    }
    return 0;
}

/* 文件系统错误必须返回给 FFmpeg，短写和 EINTR 不能被当成成功。 */
static int write_packet(void *opaque, uint8_t *buf, int size)
{
    struct ffmpeg_mjpeg_muxer *muxer = opaque;
    int done = 0;

    while (done < size) { /* 自定义 AVIO 回调负责补齐短写，不向 FFmpeg 谎报完整写入。 */
        if (muxer->cancel && __atomic_load_n(muxer->cancel, __ATOMIC_ACQUIRE)) {
            muxer->failed = 1; return AVERROR(ECANCELED); /* 会话失效后不继续写 AVIO 缓冲。 */
        }
        ssize_t n = write(muxer->file_fd, buf + done, (size_t)(size - done)); /* 从已完成偏移继续写未提交尾部。 */
        if (n < 0 && errno == EINTR) /* 信号中断尚未写入数据时重试。 */
            continue;
        if (n <= 0) { /* 零写或错误都不能当成功，避免无进展无限循环。 */
            muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
            return AVERROR(n < 0 ? errno : EIO);
        }
        done += (int)n; /* 只累计内核确认写入的字节数。 */
    }
    return done;
}

/* AVI 写尾需要回写文件头和索引，因此自定义 AVIO 必须支持 seek。 */
static int64_t seek_packet(void *opaque, int64_t offset, int whence)
{
    struct ffmpeg_mjpeg_muxer *muxer = opaque;
    struct stat64 st;
    off64_t result;

    if (whence == AVSEEK_SIZE) /* FFmpeg 特殊查询请求只返回文件大小，不移动文件位置。 */
        return fstat64(muxer->file_fd, &st) == 0 ? st.st_size : AVERROR(errno);
    result = lseek64(muxer->file_fd, (off64_t)offset, whence & ~AVSEEK_FORCE); /* AVI 索引与长度回填使用 64 位偏移，并剥离 FFmpeg 专用标志。 */
    if (result < 0) {
        muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
        return AVERROR(errno);
    }
    return result;
}

/* 上游失败后跳过正常写尾和发布，保留 .part，不把残缺录像报告为成功。 */
void ffmpeg_mjpeg_muxer_abort(struct ffmpeg_mjpeg_muxer *muxer)
{
    if (muxer != NULL)
        muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
}

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
    uint8_t *io_buffer;
    char directory[512];
    char *slash;

    if (muxer == NULL || usb_root == NULL || width == 0 || height == 0)
        return -1;

    memset(muxer, 0, sizeof(*muxer));
    muxer->file_fd = -1; /* 无效句柄用 -1 表示，避免错误关闭标准输入 fd 0。 */
    muxer->dir_fd = -1; /* 目录句柄贯穿创建、发布与目录同步，绑定同一目录对象。 */
    muxer->last_pts = -1; /* 首视频帧可以拥有 PTS 0。 */
    muxer->width = width; /* 保存压缩视频声明尺寸，不在封装层调整像素。 */
    muxer->height = height; /* 保存压缩帧高度，与摄像头实际格式一致。 */
    muxer->audio_rate = audio_rate; /* 音频样本计数转换时间使用同一采样率。 */
    muxer->audio_channels = audio_channels; /* 用于校验 PCM 字节数和计算 block_align。 */
    muxer->audio_bits = audio_bits; /* 当前音轨接受 16 位 PCM。 */
    muxer->fallback_fps = fallback_fps != 0 ? fallback_fps : 30; /* 仅提供标称帧率与 packet 缺省持续时间。 */
    muxer->frame_ms = 1000U / muxer->fallback_fps; /* 缺省单帧持续时间为整数毫秒，实际采集 PTS 优先。 */
    if (muxer->frame_ms == 0)
        muxer->frame_ms = 1; /* 防止非常高的参数产生零持续时间。 */

    if (!mounted_root(usb_root)) { /* 未实际挂载则拒绝录制，防止落入板载同名目录。 */
        fprintf(stderr, "record root is not mounted: %s\n", usb_root);
        return -1;
    }
    if (ensure_video_dir(usb_root) < 0)
        return -1;
    if (base_name == NULL || base_name[0] == '\0')
        base_name = "video";
    if (strchr(base_name, '/') != NULL || strchr(base_name, '\\') != NULL)
        return -1;

    n = snprintf(suffix, sizeof(suffix), "T113Camera/VIDEO/%s.avi", base_name);
    if (n < 0 || (size_t)n >= sizeof(suffix)) {
        fprintf(stderr, "record name too long: %s\n", base_name);
        return -1;
    }
    if (build_path(muxer->path, sizeof(muxer->path), usb_root, suffix) < 0)
        return -1;
    snprintf(muxer->temp_path, sizeof(muxer->temp_path), "%s.part", muxer->path);

    ret = avformat_alloc_output_context2(&muxer->fmt, NULL, "avi", /* 显式选择 AVI 容器，文件扩展名不是唯一判据。 */
                                         muxer->path);
    if (ret < 0 || muxer->fmt == NULL) {
        fprintf(stderr, "avformat_alloc_output_context2 failed: %d\n", ret);
        return -1;
    }

    muxer->video_stream = avformat_new_stream(muxer->fmt, NULL); /* 新建视频轨，尚未写任何压缩帧。 */
    if (muxer->video_stream == NULL) {
        fprintf(stderr, "avformat_new_stream video failed\n");
        goto fail;
    }

    muxer->video_stream->time_base = (AVRational){1, 1000}; /* 录制视频 PTS 以真实采集毫秒计，不按帧数假设固定 30fps。 */
    muxer->video_stream->avg_frame_rate = (AVRational){(int)muxer->fallback_fps, 1}; /* 标称帧率仅作元数据与缺省持续时间，真实进度由 PTS 决定。 */
    muxer->video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; /* 将此流声明为视频。 */
    muxer->video_stream->codecpar->codec_id = AV_CODEC_ID_MJPEG; /* 摄像头已输出 JPEG，直接封装，既不重编码也不调用视频编码器。 */
    muxer->video_stream->codecpar->width = (int)width; /* 视频尺寸元数据应与 MJPEG 帧一致。 */
    muxer->video_stream->codecpar->height = (int)height; /* 声明视频有效高度。 */
    muxer->video_stream->codecpar->format = AV_PIX_FMT_YUVJ422P; /* 描述 MJPEG 典型像素格式，不在这里执行像素转换。 */

    if (audio_rate > 0 && audio_channels > 0 && audio_bits == 16) { /* 仅参数完整且为 S16 时创建 PCM 音频轨。 */
        muxer->audio_stream = avformat_new_stream(muxer->fmt, NULL); /* 音频独立建轨，由 interleaved_write_frame 与视频交错。 */
        if (muxer->audio_stream == NULL) {
            fprintf(stderr, "avformat_new_stream audio failed\n");
            goto fail;
        }
        muxer->audio_stream->time_base = (AVRational){1, (int)audio_rate}; /* 音频 PTS 的一个刻度等于一个完整 PCM 样本帧。 */
        muxer->audio_stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO; /* 声明为音频而不是视频 packet。 */
        muxer->audio_stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE; /* 原始 PCM 直接放入 AVI，此链路不编码 AAC。 */
        muxer->audio_stream->codecpar->format = AV_SAMPLE_FMT_S16; /* 有符号 16 位整数音频样本格式。 */
        muxer->audio_stream->codecpar->sample_rate = (int)audio_rate; /* 文件声明必须匹配采集实际采样率，否则播放速度和音调错误。 */
        muxer->audio_stream->codecpar->channels = (int)audio_channels; /* 写入交错 PCM 的实际声道数。 */
        muxer->audio_stream->codecpar->channel_layout =
            av_get_default_channel_layout((int)audio_channels); /* 根据声道数生成 FFmpeg 默认声道布局。 */
        muxer->audio_stream->codecpar->bits_per_coded_sample = (int)audio_bits;
        muxer->audio_stream->codecpar->bits_per_raw_sample = (int)audio_bits;
        muxer->audio_stream->codecpar->block_align =
            (int)(audio_channels * audio_bits / 8U); /* block_align 为所有声道同一时刻样本的总字节数。 */
        muxer->audio_stream->codecpar->bit_rate =
            (int64_t)audio_rate * (int64_t)audio_channels * (int64_t)audio_bits; /* 未压缩 PCM 比特率由采样率、声道数和位宽相乘得到。 */
    }

    snprintf(directory, sizeof(directory), "%s", muxer->path);
    slash = strrchr(directory, '/'); /* 定位文件名之前的最后分隔符，取得目标目录。 */
    *slash = '\0';
    muxer->dir_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC); /* 打开目录用于相对创建和原子发布，避免重新按根路径寻找目标。 */
    if (muxer->dir_fd < 0)
        goto fail;
    /* 不覆盖上次失败留下的文件，需用户先检查或移走 .part。 */
    muxer->file_fd = openat(muxer->dir_fd, /* 相对已绑定目录创建 .part，避免按变化中的挂载根重寻目录。 */
                            strrchr(muxer->temp_path, '/') + 1,
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666); /* 独占创建 .part，已有失败文件绝不覆盖。 */
    if (muxer->file_fd < 0) {
        perror(muxer->temp_path);
        goto fail;
    }
    if (room_for_packet(muxer, 0) < 0) /* 写头前也检查保留空间，不等第一次大帧才发现满盘。 */
        goto fail;
    io_buffer = av_malloc(32768); /* AVIO 使用 FFmpeg 分配器创建 32KiB 聚合写缓冲。 */
    if (io_buffer == NULL)
        goto fail;
    muxer->fmt->pb = avio_alloc_context(io_buffer, 32768, 1, muxer, /* 绑定自定义写和 seek，使文件系统错误能反馈给封装器。 */
                                        NULL, write_packet, seek_packet); /* 无读回调，使用本模块可靠写入与 64 位 seek。 */
    if (muxer->fmt->pb == NULL) {
        av_free(io_buffer); /* AVIO 创建未成功时缓冲仍由本模块负责释放。 */
        goto fail;
    }
    muxer->fmt->flags |= AVFMT_FLAG_CUSTOM_IO; /* 声明 AVIO 由本模块管理，关闭时需自行释放。 */

    ret = avformat_write_header(muxer->fmt, NULL); /* 写 AVI 头；此调用也可能调整流的实际 time_base。 */
    avio_flush(muxer->fmt->pb); /* 开始录制前就发现只读盘或设备写入错误。 */
    if (ret < 0 || muxer->fmt->pb->error < 0) {
        if (ret >= 0)
            ret = muxer->fmt->pb->error; /* 写头返回成功也要检查延迟到 AVIO 层的 I/O 错误。 */
        fprintf(stderr, "avformat_write_header failed: %d\n", ret);
        goto fail;
    }
    muxer->header_written = 1; /* 仅成功写头后才允许正常写尾。 */
    muxer->start_ms = monotonic_ms(); /* 音视频使用同一个单调时钟起点。 */

    if (out_path != NULL && out_len > 0)
        snprintf(out_path, out_len, "%s", muxer->path);
    return 0;

fail:
    muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
    ffmpeg_mjpeg_muxer_close(muxer); /* 部分初始化失败统一清理，failed 标志禁止发布。 */
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

/* 兼容旧调用者；正式采集链路使用 write_at 传入采集时间。 */
int ffmpeg_mjpeg_muxer_write(struct ffmpeg_mjpeg_muxer *muxer,
                             const void *data,
                             size_t size)
{
    return ffmpeg_mjpeg_muxer_write_at(muxer, data, size,
                                       (int64_t)monotonic_ms() * 1000);
}

int ffmpeg_mjpeg_muxer_write_at(struct ffmpeg_mjpeg_muxer *muxer,
                                const void *data, size_t size, int64_t capture_us)
{
    AVPacket pkt;
    int64_t pts;
    int ret;

    if (muxer == NULL || muxer->fmt == NULL || muxer->video_stream == NULL ||
        muxer->failed || data == NULL || size == 0 || size > INT_MAX)
        return -1;

    if (room_for_packet(muxer, size) < 0)
        return -1;

    pts = capture_us / 1000 - (int64_t)muxer->start_ms; /* 绝对采集微秒减统一录制起点，转换为相对视频毫秒。 */
    if (pts < 0)
        return 0; /* 不保存录制起点前残留在 V4L2 队列里的旧帧。 */
    if (pts <= muxer->last_pts) /* 视频同毫秒或回退时间戳需修正为严格递增。 */
        pts = muxer->last_pts + 1; /* 保证写入视频 DTS 严格递增，避免重复时间戳被封装器拒绝。 */

    av_init_packet(&pkt); /* 构造临时 packet；负载借用调用者数据，写入期间必须保持有效。 */
    pkt.data = (uint8_t *)data; /* 借用压缩帧或 PCM 负载，不在封装层执行编码。 */
    pkt.size = (int)size; /* FFmpeg packet 长度为 int，入口已检查 INT_MAX 上限。 */
    pkt.stream_index = muxer->video_stream->index; /* 交错封装按此索引识别视频流。 */
    pkt.pts = pts; /* 视频显示时间来自采集时刻，而非写线程调度时刻。 */
    pkt.dts = pts; /* MJPEG 每帧独立，没有 B 帧重排，因此 DTS 等于 PTS。 */
    pkt.duration = (int64_t)muxer->frame_ms; /* 单帧缺省持续时间；相邻 PTS 仍保留真实间隔。 */
    pkt.flags = AV_PKT_FLAG_KEY; /* MJPEG 和 PCM packet 都可独立访问，标为关键包。 */
    av_packet_rescale_ts(&pkt, (AVRational){1, 1000}, muxer->video_stream->time_base); /* 写头后按容器最终视频时间基重新换算时间戳。 */

    ret = av_interleaved_write_frame(muxer->fmt, &pkt); /* FFmpeg 按音视频时间戳交错写入，负返回值使整次录制失败。 */
    if (ret < 0) {
        fprintf(stderr, "av_interleaved_write_frame failed: %d\n", ret);
        muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
        return -1;
    }

    muxer->frames++; /* 仅统计成功提交给交错封装器的视频包。 */
    muxer->bytes += size; /* 统计视频压缩负载，不包括 AVI 索引和文件头。 */
    muxer->last_pts = pts; /* 保存原始毫秒 PTS，用于下一帧单调性与索引余量检查。 */
    return 0;
}

/* AVI 的 PCM 以采样数计时，首块前补静音以保留设备启动延迟。 */
int ffmpeg_mjpeg_muxer_write_audio_at(struct ffmpeg_mjpeg_muxer *muxer,
                                      const void *data, size_t size,
                                      unsigned int frames, int64_t capture_us)
{
    int64_t target, gap;
    if (!muxer || !muxer->fmt || !muxer->audio_stream)
        return -1;
    target = (capture_us - (int64_t)muxer->start_ms * 1000) * muxer->audio_rate / 1000000; /* 将首样本采集时刻换算为相对录制起点的样本帧位置。 */
    gap = target - (int64_t)muxer->audio_frames; /* 目标样本位置与已写入数量之差，用于识别采集空洞。 */
    /* 超过 50ms 的采集空洞用静音占位，避免 XRUN 后后续声音整体提前。 */
    if ((muxer->audio_frames == 0 && gap > 0) || gap > (int64_t)muxer->audio_rate / 20) { /* 首块启动延迟或超过 50ms 的采集缺口用静音保持时间位置。 */
        unsigned char silence[4096] = {0}; /* 分块补零样本，避免按长缺口申请无界内存。 */
        unsigned int frame_bytes = muxer->audio_channels * 2; /* S16 每声道两字节，完整 PCM 帧包含全部声道。 */
        if (gap > (int64_t)muxer->audio_rate * 5 || !frame_bytes || frame_bytes > sizeof(silence)) { /* 拒绝超过五秒的异常缺口或不合理帧宽，避免无界补写。 */
            muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
            return -1;
        }
        if (muxer->audio_frames > 0)
            fprintf(stderr, "record audio gap: %lld samples padded\n", (long long)gap);
        while (gap > 0) { /* 分批写入零样本，复用栈上静音块。 */
            unsigned int count = gap < (int64_t)(sizeof(silence) / frame_bytes) ?
                                 (unsigned int)gap : sizeof(silence) / frame_bytes;
            if (ffmpeg_mjpeg_muxer_write_audio(muxer, silence, count * frame_bytes, count) < 0)
                return -1;
            gap -= count; /* 累计补齐空洞，不改变后续有效样本的相对时刻。 */
        }
    }
    return ffmpeg_mjpeg_muxer_write_audio(muxer, data, size, frames);
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
        muxer->failed || data == NULL || size == 0 || size > INT_MAX || frames == 0)
        return -1;
    if (size != (size_t)frames * muxer->audio_channels * 2U) { /* 字节数必须与帧数和声道匹配，拒绝损坏的 PCM 描述符。 */
        muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
        return -1;
    }
    if (room_for_packet(muxer, size) < 0)
        return -1;

    av_init_packet(&pkt); /* 构造临时 packet；负载借用调用者数据，写入期间必须保持有效。 */
    pkt.data = (uint8_t *)data; /* 借用压缩帧或 PCM 负载，不在封装层执行编码。 */
    pkt.size = (int)size; /* FFmpeg packet 长度为 int，入口已检查 INT_MAX 上限。 */
    pkt.stream_index = muxer->audio_stream->index; /* 将原始 PCM 包归入音频轨。 */
    pkt.pts = (int64_t)muxer->audio_frames; /* 音频连续样本计数即其 PTS。 */
    pkt.dts = (int64_t)muxer->audio_frames; /* PCM 没有解码重排，DTS 与 PTS 相同。 */
    pkt.duration = (int64_t)frames; /* 包持续时间以完整样本帧数表达，不是字节数。 */
    pkt.flags = AV_PKT_FLAG_KEY; /* MJPEG 和 PCM packet 都可独立访问，标为关键包。 */
    av_packet_rescale_ts(&pkt, (AVRational){1, (int)muxer->audio_rate}, /* 将样本帧计数换算为容器写头后确定的音频时间基。 */
                         muxer->audio_stream->time_base);

    ret = av_interleaved_write_frame(muxer->fmt, &pkt); /* FFmpeg 按音视频时间戳交错写入，负返回值使整次录制失败。 */
    if (ret < 0) {
        fprintf(stderr, "av_interleaved_write_frame audio failed: %d\n", ret);
        muxer->failed = 1; /* 失败状态粘滞，后续 close 不得把部分录像发布为完整 AVI。 */
        return -1;
    }

    muxer->audio_blocks++; /* 统计实际写入块数，包含时间线补入的静音块。 */
    muxer->audio_frames += frames; /* 推进音频样本时间线，包含补入的静音。 */
    muxer->audio_bytes += size; /* 累计未压缩 PCM 有效负载。 */
    return 0;
}

/* 排空 FFmpeg 交错缓冲、写尾、fsync，成功后才发布正式文件。 */
int ffmpeg_mjpeg_muxer_close(struct ffmpeg_mjpeg_muxer *muxer)
{
    int ret = 0;

    if (muxer == NULL)
        return 0;

    if (muxer->cancel && __atomic_load_n(muxer->cancel, __ATOMIC_ACQUIRE))
        muxer->failed = 1;          /* 不为失效会话写正常文件尾。 */

    if (muxer->fmt != NULL) {
        if (muxer->fmt->pb != NULL) {
            if (!muxer->failed && muxer->header_written && av_write_trailer(muxer->fmt) < 0) /* 正常停止才写 AVI 索引和文件尾；失败会保留 .part。 */
                ret = -1;
            avio_flush(muxer->fmt->pb); /* 将 FFmpeg 用户态缓存交给自定义写回调，仍不等于落盘完成。 */
            if (muxer->fmt->pb->error < 0) /* flush 后再次核对底层写错误，不能只看 trailer 返回值。 */
                ret = -1;
            av_freep(&muxer->fmt->pb->buffer); /* 释放 AVIO 当前缓冲；FFmpeg 可能已替换最初分配的指针。 */
            avio_context_free(&muxer->fmt->pb); /* 再释放 AVIO 上下文。 */
        }
        if (muxer->file_fd >= 0) {
            if (!muxer->failed && fsync(muxer->file_fd) < 0) { /* 完整写尾后请求文件数据持久化；错误时不得发布正式录像。 */
                perror("record fsync");
                ret = -1;
            }
            if (close(muxer->file_fd) < 0) /* 关闭也可能报告延迟 I/O 错误，必须参与最终保存结果。 */
                ret = -1;
            muxer->file_fd = -1; /* 无效句柄用 -1 表示，避免错误关闭标准输入 fd 0。 */
        }
        if (!muxer->header_written || muxer->frames == 0 || muxer->failed ||
            (muxer->cancel && __atomic_load_n(muxer->cancel, __ATOMIC_ACQUIRE)) ||
            (muxer->audio_stream != NULL && muxer->audio_frames == 0))
            ret = -1;
        if (ret == 0) {
            if (media_publish_noreplace(muxer->dir_fd, strrchr(muxer->temp_path, '/') + 1, /* 只在写尾和数据同步均成功后原子发布正式名称。 */
                                        muxer->dir_fd, strrchr(muxer->path, '/') + 1) < 0) {
                perror("record rename");
                ret = -1;
            } else if (fsync(muxer->dir_fd) < 0) { /* 原子改名后同步目录项，完成发布持久化请求。 */
                perror("record directory fsync");
                ret = -1;
            }
        }
        if (muxer->dir_fd >= 0 && close(muxer->dir_fd) < 0)
            ret = -1;
        muxer->dir_fd = -1; /* 目录句柄贯穿创建、发布与目录同步，绑定同一目录对象。 */
        if (ret < 0)
            fprintf(stderr, "record not committed: %s (check .part if present)\n",
                    muxer->path);
        avformat_free_context(muxer->fmt); /* 释放容器与流元数据，文件和自定义 AVIO 已先清理。 */
    }

    muxer->fmt = NULL; /* 防止重复 close 使用已释放的上下文。 */
    muxer->video_stream = NULL; /* 流对象由容器上下文拥有，不能单独 free。 */
    muxer->audio_stream = NULL; /* 容器释放后清除音轨借用指针。 */
    muxer->failed |= ret < 0; /* 失败状态保持粘滞，后续清理不能把失败录像误报成功。 */
    return ret;
}
