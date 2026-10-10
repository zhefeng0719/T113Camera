#include "record_writer.h"
#include "media_clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static int video_count, audio_count, aborted, slow;
int ffmpeg_mjpeg_muxer_write_at(struct ffmpeg_mjpeg_muxer *muxer,
                               const void *data, size_t size, int64_t pts_us)
{
    (void)muxer; (void)data; (void)size; (void)pts_us;
    if (slow)
        usleep(200000);
    video_count++;
    return 0;
}
int ffmpeg_mjpeg_muxer_write_audio_at(struct ffmpeg_mjpeg_muxer *muxer,
                                     const void *data, size_t size, unsigned int frames, int64_t pts_us)
{
    (void)muxer; (void)data; (void)size; (void)frames; (void)pts_us;
    audio_count++;
    return 0;
}
void ffmpeg_mjpeg_muxer_abort(struct ffmpeg_mjpeg_muxer *muxer)
{
    (void)muxer;
    aborted = 1;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\n", __LINE__); return 1; } } while (0)
int main(void)
{
    struct record_writer writer;
    struct ffmpeg_mjpeg_muxer muxer = {0};
    struct audio_queue audio;
    struct audio_block block = {.data = malloc(16), .bytes = 16, .frames = 8, .pts_us = 1000};
    unsigned char jpeg[128] = {0};
    int full = 0;
    int64_t start;
    CHECK(block.data && audio_queue_init(&audio, 16) == 0);
    CHECK(record_writer_start(&writer, &muxer, &audio) == 0);
    CHECK(audio_queue_push(&audio, &block) == 0);
    CHECK(record_writer_submit(&writer, jpeg, sizeof(jpeg), 1000) == 0);
    CHECK(record_writer_submit(&writer, jpeg, sizeof(jpeg), 2000) == 0);
    audio_queue_stop(&audio);
    CHECK(record_writer_finish(&writer) == 0);
    CHECK(video_count == 2 && audio_count == 1 && !aborted);
    audio_queue_destroy(&audio);
    CHECK(audio_queue_init(&audio, 16) == 0);
    slow = 1;
    CHECK(record_writer_start(&writer, &muxer, &audio) == 0);
    start = media_monotonic_ms();
    for (int i = 0; i < 32; i++) {
        if (record_writer_submit(&writer, jpeg, sizeof(jpeg), i * 1000) < 0) {
            full = 1;
            break;
        }
    }
    CHECK(full && media_monotonic_ms() - start < 100);
    audio_queue_stop(&audio);
    CHECK(record_writer_finish(&writer) < 0 && aborted);
    audio_queue_destroy(&audio);
    volatile int cancel = 0; /* 模拟录制中 U 盘会话失效，而非真正拔出设备。 */
    CHECK(audio_queue_init(&audio, 16) == 0);
    aborted = 0; slow = 0; /* 避免上一组饱和测试影响此断言。 */
    CHECK(record_writer_start_cancelable(&writer, &muxer, &audio, &cancel) == 0);
    __atomic_store_n(&cancel, 1, __ATOMIC_RELEASE); /* 监控线程同样使用原子写入。 */
    usleep(10000); /* 等待写线程读到取消并停止音频生产者。 */
    CHECK(record_writer_finish(&writer) < 0 && aborted); /* 取消不能返回保存成功。 */
    audio_queue_destroy(&audio);
    puts("Writer drain, nonblocking saturation and session cancellation: PASS");
    return 0;
}
