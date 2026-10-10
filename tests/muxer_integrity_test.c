/* 只测试封装和故障处理，不打开摄像头、声卡或显示设备。 */
#include "ffmpeg_mjpeg_muxer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

enum fault { NONE, WRITE_ERROR, FILE_SYNC_ERROR, DIR_SYNC_ERROR,
             RENAME_ERROR, LOW_SPACE };
static enum fault injected;
static int sync_calls;
static int failures;

ssize_t __real_write(int fd, const void *data, size_t size);
int __real_fsync(int fd);
int __real_media_publish_noreplace(int oldfd, const char *oldpath, int newfd, const char *newpath);
int __real_fstatvfs(int fd, struct statvfs *space);

ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    if (injected == WRITE_ERROR) {
        errno = EIO;
        return -1;
    }
    return __real_write(fd, data, size);
}

int __wrap_fsync(int fd)
{
    sync_calls++;
    if ((injected == FILE_SYNC_ERROR && sync_calls == 1) ||
        (injected == DIR_SYNC_ERROR && sync_calls == 2)) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __wrap_media_publish_noreplace(int oldfd, const char *oldpath, int newfd, const char *newpath)
{
    if (injected == RENAME_ERROR) {
        errno = EIO;
        return -1;
    }
    return __real_media_publish_noreplace(oldfd, oldpath, newfd, newpath);
}

int __wrap_fstatvfs(int fd, struct statvfs *space)
{
    int ret = __real_fstatvfs(fd, space);
    if (ret == 0 && injected == LOW_SPACE)
        space->f_bavail = 0;
    return ret;
}

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); failures++; \
} } while (0)

/* 重读容器，检查队尾的 PCM 和视频 packet 是否都实际写入。 */
static void verify_packets(const char *path)
{
    AVFormatContext *fmt = NULL;
    AVPacket packet;
    int video = 0, audio = 0, ret;

    ret = avformat_open_input(&fmt, path, NULL, NULL);
    CHECK(ret == 0);
    if (ret < 0)
        return;
    av_init_packet(&packet);
    while ((ret = av_read_frame(fmt, &packet)) >= 0) {
        if (fmt->streams[packet.stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            video++;
        else
            audio += packet.size;
        av_packet_unref(&packet);
    }
    CHECK(ret == AVERROR_EOF);
    CHECK(video == 3);
    CHECK(audio == 3 * 3200);
    avformat_close_input(&fmt);
}

/* 实际摄像头录像检查：解析头、读到 EOF，并统计两路 packet。 */
static int inspect_recording(const char *path)
{
    AVFormatContext *fmt = NULL;
    AVPacket packet;
    uint64_t packets[2] = {0}, bytes[2] = {0};
    int ret, i;
    int64_t previous[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    int64_t video_end_ms = 0;
    uint64_t pcm_frames = 0;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0)
        return 1;
    CHECK(avformat_find_stream_info(fmt, NULL) >= 0);
    CHECK(fmt->nb_streams == 2);
    for (i = 0; i < (int)fmt->nb_streams; i++) {
        AVStream *stream = fmt->streams[i];
        printf("stream %d codec=%d duration=%lld time_base=%d/%d rate=%d channels=%d\n",
               i, stream->codecpar->codec_id, (long long)stream->duration,
               stream->time_base.num, stream->time_base.den,
               stream->codecpar->sample_rate, stream->codecpar->channels);
    }
    av_init_packet(&packet);
    while ((ret = av_read_frame(fmt, &packet)) >= 0) {
        i = packet.stream_index;
        if (i >= 0 && i < 2) {
            if (packet.dts != AV_NOPTS_VALUE && previous[i] != AV_NOPTS_VALUE)
                CHECK(packet.dts >= previous[i]);
            previous[i] = packet.dts;
            packets[i]++;
            bytes[i] += packet.size;
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && packet.pts != AV_NOPTS_VALUE)
                video_end_ms = av_rescale_q(packet.pts + packet.duration,
                                            fmt->streams[i]->time_base, (AVRational){1,1000});
            if (fmt->streams[i]->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE)
                pcm_frames += packet.size / (fmt->streams[i]->codecpar->channels * 2);
        }
        av_packet_unref(&packet);
    }
    CHECK(ret == AVERROR_EOF);
    CHECK(packets[0] > 0 && packets[1] > 0);
    for (i = 0; i < 2; i++)
        printf("stream %d packets=%llu bytes=%llu\n", i,
               (unsigned long long)packets[i], (unsigned long long)bytes[i]);
    if (pcm_frames) {
        int rate = 0;
        for (i = 0; i < (int)fmt->nb_streams; i++)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                rate = fmt->streams[i]->codecpar->sample_rate;
        printf("duration: video_end=%lldms pcm=%llums tail_difference=%lldms\n",
               (long long)video_end_ms, (unsigned long long)(pcm_frames * 1000 / rate),
               (long long)video_end_ms - (int64_t)(pcm_frames * 1000 / rate));
    }
    avformat_close_input(&fmt);
    printf("INSPECT: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *names[] = {"ok", "abort", "empty", "write", "fsync",
                           "rename", "space", "dirsync", "header", "conflict", "cancel"};
    struct ffmpeg_mjpeg_muxer muxer;
    unsigned char jpeg[1024] = {0xff, 0xd8, 0xff, 0xd9};
    short pcm[1600] = {0};
    char name[80], path[512], partial[520], saved_path[512];
    struct stat before, after;
    int i, j, ret;

    if (argc == 3 && strcmp(argv[1], "--inspect") == 0)
        return inspect_recording(argv[2]);
    if (argc != 2) {
        fprintf(stderr, "usage: %s <mounted test root, e.g. /tmp>\n", argv[0]);
        return 2;
    }
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        volatile int cancel = 0; /* 模拟拔盘取消，标志生命周期覆盖 close。 */
        injected = NONE;
        sync_calls = 0;
        snprintf(name, sizeof(name), "integrity_%ld_%s", (long)getpid(), names[i]);
        if (i == 8)
            injected = WRITE_ERROR;
        ret = ffmpeg_mjpeg_muxer_open_av_named(&muxer, argv[1], name,
                                               640, 480, 30, 48000, 1, 16,
                                               path, sizeof(path));
        if (i == 8) {
            CHECK(ret < 0);
            injected = NONE;
            CHECK(access(muxer.path, F_OK) < 0);
            CHECK(access(muxer.temp_path, F_OK) == 0);
            unlink(muxer.temp_path);
            printf("case header   checked\n");
            continue;
        }
        CHECK(ret == 0);
        if (ret < 0)
            continue;
        snprintf(partial, sizeof(partial), "%s.part", path);
        CHECK(access(path, F_OK) < 0);
        CHECK(access(partial, F_OK) == 0);
        if (i != 2) {
            for (j = 0; j < 3; j++) {
                CHECK(ffmpeg_mjpeg_muxer_write(&muxer, jpeg, sizeof(jpeg)) == 0);
                CHECK(ffmpeg_mjpeg_muxer_write_audio(&muxer, pcm, sizeof(pcm), 1600) == 0);
                usleep(33333);
            }
        }
        if (i == 1)
            ffmpeg_mjpeg_muxer_abort(&muxer);
        if (i == 3)
            injected = WRITE_ERROR;
        if (i == 4)
            injected = FILE_SYNC_ERROR;
        if (i == 5)
            injected = RENAME_ERROR;
        if (i == 6) {
            injected = LOW_SPACE;
            CHECK(ffmpeg_mjpeg_muxer_write(&muxer, jpeg, sizeof(jpeg)) < 0);
        }
        if (i == 7)
            injected = DIR_SYNC_ERROR;
        if (i == 9) {
            FILE *other = fopen(path, "wb");
            CHECK(other != NULL);
            if (other) {
                CHECK(fwrite("KEEP", 1, 4, other) == 4);
                fclose(other);
            }
        }
        if (i == 10) {
            muxer.cancel = &cancel; /* 不依赖存储模块也能注入失效会话。 */
            __atomic_store_n(&cancel, 1, __ATOMIC_RELEASE); /* 最终发布前发生取消。 */
        }
        ret = ffmpeg_mjpeg_muxer_close(&muxer);
        injected = NONE;
        CHECK(i == 0 ? ret == 0 : ret < 0);
        if (i == 9) {
            char marker[4] = {0};
            FILE *other = fopen(path, "rb");
            CHECK(other != NULL);
            if (other) {
                CHECK(fread(marker, 1, 4, other) == 4 && memcmp(marker, "KEEP", 4) == 0);
                fclose(other);
            }
            CHECK(access(partial, F_OK) == 0);
        } else if (i == 0 || i == 7) {
            /* 改名后的目录 fsync 失败必须报告失败，但文件可能已经可见。 */
            CHECK(access(path, F_OK) == 0);
            CHECK(access(partial, F_OK) < 0);
            verify_packets(path);
        } else {
            CHECK(access(path, F_OK) < 0);
            CHECK(access(partial, F_OK) == 0);
            /* 残留 .part 不应被下一次尝试截断或覆盖。 */
            CHECK(stat(partial, &before) == 0);
            CHECK(ffmpeg_mjpeg_muxer_open_named(&muxer, argv[1], name,
                                                640, 480, 30, NULL, 0) < 0);
            CHECK(stat(partial, &after) == 0 && before.st_size == after.st_size);
        }
        if (i == 0) {
            snprintf(saved_path, sizeof(saved_path), "%s", path);
            CHECK(stat(path, &before) == 0);
            CHECK(ffmpeg_mjpeg_muxer_open_named(&muxer, argv[1], name,
                                                640, 480, 30, NULL, 0) == 0);
            CHECK(ffmpeg_mjpeg_muxer_write(&muxer, jpeg, sizeof(jpeg)) == 0);
            ffmpeg_mjpeg_muxer_abort(&muxer);
            CHECK(ffmpeg_mjpeg_muxer_close(&muxer) < 0);
            CHECK(stat(saved_path, &after) == 0 && before.st_size == after.st_size);
            verify_packets(saved_path);
        }
        unlink(path);
        unlink(partial);
        printf("case %-8s checked\n", names[i]);
    }
    /* 非挂载点和非法名称不能创建录像。 */
    CHECK(ffmpeg_mjpeg_muxer_open(&muxer, "/no-such-t113-mount", 640, 480, 30,
                                  NULL, 0) < 0);
    CHECK(ffmpeg_mjpeg_muxer_open_named(&muxer, argv[1], "../invalid", 640, 480, 30,
                                        NULL, 0) < 0);
    printf("RESULT: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
