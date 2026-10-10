#include "jpeg_info.h"
#include "audio_timeline.h"
#include "ffmpeg_io.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d\n", __LINE__); return 1; } } while (0)
int main(void)
{
    unsigned int w, h, gap, skip;
    size_t offset = 0, size;
    const unsigned char *frame;
    /* APP payload contains a thumbnail EOI; it must not truncate the outer JPEG. */
    unsigned char jpeg[] = {255,216,255,225,0,6,255,216,255,217,
                            255,192,0,11,8,1,224,2,128,1,1,17,0,
                            255,218,0,2,12,255,0,21,255,217};
    volatile int stop = 0;
    struct ffmpeg_io_guard io = {.stop = &stop};
    CHECK(jpeg_dimensions(jpeg, sizeof(jpeg), &w, &h) == 0 && w == 640 && h == 480);
    CHECK(jpeg_next_frame(jpeg, sizeof(jpeg), &offset, &frame, &size) == 1);
    CHECK(frame == jpeg && size == sizeof(jpeg));
    offset = 0;
    CHECK(jpeg_next_frame(jpeg, sizeof(jpeg) - 1, &offset, &frame, &size) == 0);
    jpeg[4] = 127;
    CHECK(jpeg_dimensions(jpeg, sizeof(jpeg), &w, &h) < 0);
    CHECK(audio_timeline_plan(48000, 24000, 48000, 1024, &gap, &skip) == 0 && gap == 24000 && !skip);
    CHECK(audio_timeline_plan(24000, 24500, 48000, 1024, &gap, &skip) == 0 && !gap && skip == 500);
    CHECK(audio_timeline_plan(24000, 26000, 48000, 1024, &gap, &skip) == 0 && skip == 1024);
    CHECK(audio_timeline_plan(4800000, 0, 48000, 1024, &gap, &skip) < 0);
    ffmpeg_io_begin(&io);
    CHECK(!ffmpeg_io_interrupt(&io));
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    CHECK(ffmpeg_io_interrupt(&io));
    stop = 0;
    io.deadline_us = ffmpeg_io_now() - 1;
    CHECK(ffmpeg_io_interrupt(&io));
    puts("JPEG boundaries/dimensions, audio timeline, I/O guards: PASS");
    return 0;
}
