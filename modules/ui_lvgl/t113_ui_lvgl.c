/*
 * t113_ui_lvgl.c
 *
 * 把 GUI Guider 生成的 LVGL 界面渲染到全志 ION/dmabuf。
 *
 * 当前职责很小：
 *   LVGL draw buffer -> ION ARGB8888 dmabuf
 *   由外层 /dev/disp 模块把这个 dmabuf 显示到屏幕
 */

#include "t113_ui_lvgl.h"

#include "custom.h"
#include "gg_utils.h"
#include "gui_guider.h"
#include "lvgl.h"

#include <ion_mem_alloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define T113_UI_DRAW_LINES 80

gg_ui_t guider_ui;
static struct t113_ui_lvgl *g_flush_target;

/* LVGL 使用毫秒节拍，这里用单调时钟避免受系统时间变化影响。 */
static uint32_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000U + ts.tv_nsec / 1000000U);
}

/* LVGL 的 flush 回调：把局部刷新区域拷贝到 ION 显示缓冲。 */
static void ui_flush_cb(lv_display_t *display,
                        const lv_area_t *area,
                        uint8_t *px_map)
{
    struct t113_ui_lvgl *ui = lv_display_get_user_data(display);
    int32_t width;
    int32_t x;
    int32_t y;
    uint32_t *src;

    if (ui == NULL || ui->virt == NULL || area == NULL || px_map == NULL) {
        lv_display_flush_ready(display);
        return;
    }

    if (area->x2 < 0 || area->y2 < 0 ||
        area->x1 >= (int32_t)ui->width ||
        area->y1 >= (int32_t)ui->height) {
        lv_display_flush_ready(display);
        return;
    }

    width = area->x2 - area->x1 + 1;
    src = (uint32_t *)px_map;

    for (y = area->y1; y <= area->y2; y++) {
        if (y < 0 || y >= (int32_t)ui->height) {
            src += width;
            continue;
        }

        for (x = area->x1; x <= area->x2; x++) {
            uint32_t pixel = *src++;

            if (x >= 0 && x < (int32_t)ui->width) {
                uint32_t *dst = (uint32_t *)((uint8_t *)ui->virt +
                                             (size_t)y * ui->line_length +
                                             (size_t)x * ui->bytes_per_pixel);
                *dst = pixel | 0xff000000U;
            }
        }
    }

    SunxiMemFlushCache(ui->memops, ui->virt, (int)ui->size);
    ui->flush_count++;
    lv_display_flush_ready(display);
}

/* 创建 LVGL display，并绑定到 ION/dmabuf 后端。 */
static lv_display_t *ui_display_create(struct t113_ui_lvgl *ui)
{
    lv_display_t *display;
    static uint8_t draw_buf[800 * T113_UI_DRAW_LINES * 4];

    display = lv_display_create((int32_t)ui->width, (int32_t)ui->height);
    if (display == NULL)
        return NULL;

    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_user_data(display, ui);
    lv_display_set_flush_cb(display, ui_flush_cb);
    lv_display_set_buffers(display, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(display);
    return display;
}

/* 分配一块可给 /dev/disp 扫描的 ARGB8888 ION 缓冲。 */
static int ui_alloc_dmabuf(struct t113_ui_lvgl *ui)
{
    ui->bytes_per_pixel = 4;
    ui->line_length = ui->width * ui->bytes_per_pixel;
    ui->size = (size_t)ui->line_length * ui->height;

    ui->memops = GetMemAdapterOpsS();
    if (ui->memops == NULL) {
        fprintf(stderr, "GetMemAdapterOpsS failed\n");
        return -1;
    }

    if (SunxiMemOpen(ui->memops) < 0) {
        fprintf(stderr, "SunxiMemOpen failed\n");
        ui->memops = NULL;
        return -1;
    }

    ui->virt = SunxiMemPalloc(ui->memops, (int)ui->size);
    if (ui->virt == NULL) {
        fprintf(stderr, "SunxiMemPalloc failed, size=%lu\n",
                (unsigned long)ui->size);
        SunxiMemClose(ui->memops);
        ui->memops = NULL;
        return -1;
    }

    ui->dmabuf_fd = SunxiMemGetBufferFd(ui->memops, ui->virt);
    if (ui->dmabuf_fd < 0) {
        fprintf(stderr, "SunxiMemGetBufferFd failed\n");
        SunxiMemPfree(ui->memops, ui->virt);
        SunxiMemClose(ui->memops);
        ui->virt = NULL;
        ui->memops = NULL;
        return -1;
    }

    memset(ui->virt, 0, ui->size);
    SunxiMemFlushCache(ui->memops, ui->virt, (int)ui->size);
    return 0;
}

int t113_ui_lvgl_open(struct t113_ui_lvgl *ui,
                      uint32_t width,
                      uint32_t height)
{
    if (ui == NULL)
        return -1;

    memset(ui, 0, sizeof(*ui));
    ui->dmabuf_fd = -1;
    ui->width = width;
    ui->height = height;

    if (width != 800U || height != 480U) {
        fprintf(stderr, "lvgl ui only verified at 800x480, got %ux%u\n",
                width, height);
        return -1;
    }

    if (ui_alloc_dmabuf(ui) < 0)
        return -1;

    lv_init();
    lv_tick_set_cb(monotonic_ms);
    g_flush_target = ui;

    if (ui_display_create(ui) == NULL) {
        fprintf(stderr, "lv_display_create failed\n");
        t113_ui_lvgl_close(ui);
        return -1;
    }

    setup_ui(&guider_ui);
    custom_init(&guider_ui);
    t113_gui_set_title("T113Camera", "LVGL on T113S3");
    t113_gui_set_playing(false);
    t113_gui_set_progress(0, 100);
    t113_gui_set_usb_status("USB NOT READY");
    t113_gui_set_fps(0);
    t113_gui_set_rec_led(0);
    t113_gui_set_video_message("");

    lv_timer_handler();
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    return 0;
}

void t113_ui_lvgl_tick(struct t113_ui_lvgl *ui)
{
    (void)ui;
    t113_gui_audio_visualizer_tick();
    lv_timer_handler();
}

void t113_ui_lvgl_set_playing(bool playing)
{
    t113_gui_set_playing(playing);
}

void t113_ui_lvgl_set_screen_title(const char *text)
{
    t113_gui_set_screen_title(text);
}

void t113_ui_lvgl_set_progress(uint32_t value, uint32_t max)
{
    t113_gui_set_progress(value, max);
}

void t113_ui_lvgl_set_title(const char *title, const char *subtitle)
{
    t113_gui_set_title(title, subtitle);
}

void t113_ui_lvgl_set_usb_status(const char *text)
{
    t113_gui_set_usb_status(text);
}

void t113_ui_lvgl_set_fps(uint32_t fps)
{
    t113_gui_set_fps(fps);
}

void t113_ui_lvgl_set_rec_led(uint8_t brightness)
{
    t113_gui_set_rec_led(brightness);
}

void t113_ui_lvgl_set_video_message(const char *text)
{
    t113_gui_set_video_message(text);
}

void t113_ui_lvgl_set_audio_visualizer(bool show)
{
    t113_gui_set_audio_visualizer(show);
}

void t113_ui_lvgl_playlist_clear(void)
{
    t113_gui_playlist_clear();
}

bool t113_ui_lvgl_playlist_add(const char *text)
{
    return t113_gui_playlist_add(text);
}

void t113_ui_lvgl_playlist_select(uint32_t index)
{
    t113_gui_playlist_select(index);
}

bool t113_ui_lvgl_get_video_rect(struct t113_ui_rect *rect)
{
    t113_gui_rect_t gui_rect;

    if (rect == NULL)
        return false;
    if (!t113_gui_get_video_content_rect(&gui_rect))
        return false;

    rect->x = gui_rect.x;
    rect->y = gui_rect.y;
    rect->w = gui_rect.w;
    rect->h = gui_rect.h;
    return true;
}

int t113_ui_lvgl_get_dmabuf_fd(const struct t113_ui_lvgl *ui)
{
    return ui ? ui->dmabuf_fd : -1;
}

uint32_t t113_ui_lvgl_get_width(const struct t113_ui_lvgl *ui)
{
    return ui ? ui->width : 0U;
}

uint32_t t113_ui_lvgl_get_height(const struct t113_ui_lvgl *ui)
{
    return ui ? ui->height : 0U;
}

uint32_t t113_ui_lvgl_get_flush_count(const struct t113_ui_lvgl *ui)
{
    return ui ? ui->flush_count : 0U;
}

void t113_ui_lvgl_close(struct t113_ui_lvgl *ui)
{
    if (ui == NULL)
        return;

    if (g_flush_target == ui)
        g_flush_target = NULL;
    if (ui->virt != NULL && ui->memops != NULL)
        SunxiMemPfree(ui->memops, ui->virt);
    if (ui->memops != NULL)
        SunxiMemClose(ui->memops);

    memset(ui, 0, sizeof(*ui));
    ui->dmabuf_fd = -1;
}

void t113_gui_on_play(void)
{
    printf("ui event    : play\n");
}

void t113_gui_on_pause(void)
{
    printf("ui event    : pause\n");
}

void t113_gui_on_previous(void)
{
    printf("ui event    : previous\n");
}

void t113_gui_on_next(void)
{
    printf("ui event    : next\n");
}
