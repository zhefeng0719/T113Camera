#ifndef T113_UI_LVGL_H
#define T113_UI_LVGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* GUI Guider 中 vedioplay 控件对应的矩形区域。 */
struct t113_ui_rect {
    int x;
    int y;
    int w;
    int h;
};

/* LVGL 渲染到 ION/dmabuf 的运行上下文。 */
struct t113_ui_lvgl {
    struct SunxiMemOpsS *memops;
    void *virt;
    int dmabuf_fd;
    uint32_t width;
    uint32_t height;
    uint32_t line_length;
    uint32_t bytes_per_pixel;
    uint32_t flush_count;
    size_t size;
};

/* 初始化 LVGL，并把 GUI Guider 生成的界面渲染到 dmabuf。 */
int t113_ui_lvgl_open(struct t113_ui_lvgl *ui,
                      uint32_t width,
                      uint32_t height);

/* 运行一次 LVGL 定时器，用于处理动画、事件和局部刷新。 */
void t113_ui_lvgl_tick(struct t113_ui_lvgl *ui);

/* 设置播放按钮状态。 */
void t113_ui_lvgl_set_playing(bool playing);

/* 设置 GUI Guider 顶部 TITLE 控件文字。 */
void t113_ui_lvgl_set_screen_title(const char *text);

/* 设置进度条位置。 */
void t113_ui_lvgl_set_progress(uint32_t value, uint32_t max);

/* 设置界面标题。 */
void t113_ui_lvgl_set_title(const char *title, const char *subtitle);

/* 设置 U 盘状态文字。 */
void t113_ui_lvgl_set_usb_status(const char *text);

/* 设置 FPS 标签。 */
void t113_ui_lvgl_set_fps(uint32_t fps);

/* 设置录制 LED 亮度，0 熄灭，255 最亮。 */
void t113_ui_lvgl_set_rec_led(uint8_t brightness);

/* 设置视频显示区域中间的提示文字，例如 NO SUPPORT。 */
void t113_ui_lvgl_set_video_message(const char *text);

/* 显示或隐藏视频框中的音频可视化条。 */
void t113_ui_lvgl_set_audio_visualizer(bool show);

/* 清空播放列表。 */
void t113_ui_lvgl_playlist_clear(void);

/* 添加一个播放列表条目。 */
bool t113_ui_lvgl_playlist_add(const char *text);

/* 高亮播放列表中的当前条目。 */
void t113_ui_lvgl_playlist_select(uint32_t index);

/* 读取 vedioplay 控件内部内容区域，后续视频层可以对齐这个区域。 */
bool t113_ui_lvgl_get_video_rect(struct t113_ui_rect *rect);

/* 获取 LVGL 当前渲染出的 ARGB8888 dmabuf fd。 */
int t113_ui_lvgl_get_dmabuf_fd(const struct t113_ui_lvgl *ui);

/* 获取 dmabuf 宽度。 */
uint32_t t113_ui_lvgl_get_width(const struct t113_ui_lvgl *ui);

/* 获取 dmabuf 高度。 */
uint32_t t113_ui_lvgl_get_height(const struct t113_ui_lvgl *ui);

/* 获取 LVGL flush 次数，用于判断画面是否有更新。 */
uint32_t t113_ui_lvgl_get_flush_count(const struct t113_ui_lvgl *ui);

/* 释放 LVGL 显示缓冲和 ION 资源。 */
void t113_ui_lvgl_close(struct t113_ui_lvgl *ui);

#endif
