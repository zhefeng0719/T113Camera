/* 验证真实 ALSA 主时钟冻结、恢复和中断，不调用 LVGL。 */
#include "ffmpeg_audio_player.h"
#include <stdio.h>
#include <unistd.h>

struct worker {
    const char *path;
    struct media_clock clock;
    volatile int stop, paused;
    int done, ret;
};
static void *play(void *arg)
{
    struct worker *w = arg;
    w->ret = ffmpeg_audio_play_clock(w->path, "hw:0,0", &w->stop, &w->paused,
                                      NULL, NULL, &w->clock);
    __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
    return NULL;
}
int main(int argc, char **argv)
{
    struct worker w = {0};
    pthread_t thread;
    int64_t first = 0, held = 0, resumed = 0, start, latency;
    int fail = 0;
    if (argc != 2)
        return 2;
    w.path = argv[1];
    if (media_clock_init(&w.clock) || pthread_create(&thread, NULL, play, &w))
        return 1;
    start = media_monotonic_ms();
    while (!media_clock_read(&w.clock, &first) || first < 500) {
        if (__atomic_load_n(&w.done, __ATOMIC_ACQUIRE) || media_monotonic_ms() - start > 5000) {
            fail = 1;
            break;
        }
        usleep(5000);
    }
    __atomic_store_n(&w.paused, 1, __ATOMIC_RELEASE);
    usleep(150000); /* 给设备暂停或软件保存缓冲留出响应时间。 */
    media_clock_read(&w.clock, &first);
    usleep(500000);
    media_clock_read(&w.clock, &held);
    if (held != first)
        fail = 1;
    __atomic_store_n(&w.paused, 0, __ATOMIC_RELEASE);
    usleep(300000);
    media_clock_read(&w.clock, &resumed);
    if (resumed < held + 200 || resumed > held + 400)
        fail = 1;
    start = media_monotonic_ms();
    __atomic_store_n(&w.stop, 1, __ATOMIC_RELEASE);
    pthread_join(thread, NULL);
    latency = media_monotonic_ms() - start;
    if (latency > 300 || w.ret)
        fail = 1;
    printf("pause=%lld->%lld resume=%lld stop_latency=%lldms ret=%d\n",
           (long long)first, (long long)held, (long long)resumed, (long long)latency, w.ret);
    media_clock_destroy(&w.clock);
    printf("RESULT: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
