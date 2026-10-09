/*
 * Copyright 2026 NXP
 * NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
 * accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
 * activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
 * comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
 * terms, then you may not retain, install, activate or otherwise use the software.
 */

#include "custom.h"
#include "spectrum_2.h"

#if defined(__GNUC__)
#define T113_WEAK __attribute__((weak))
#else
#define T113_WEAK
#endif

static gg_ui_t *s_ui;
static lv_obj_t *s_video_message;
static lv_obj_t *s_audio_bars[16];
static lv_obj_t *s_playlist_buttons[64];
static uint32_t s_playlist_count;
static uint32_t s_audio_frame;
static bool s_audio_visualizer_show;

#define T113_SPECTRUM_COUNT ((uint32_t)(sizeof(spectrum_2) / sizeof(spectrum_2[0])))
#define T113_AUDIO_BAR_COUNT ((uint32_t)(sizeof(s_audio_bars) / sizeof(s_audio_bars[0])))

void T113_WEAK t113_gui_on_play(void)
{
}

void T113_WEAK t113_gui_on_pause(void)
{
}

void T113_WEAK t113_gui_on_previous(void)
{
}

void T113_WEAK t113_gui_on_next(void)
{
}

void custom_init(gg_ui_t *ui)
{
    s_ui = ui;
    s_video_message = NULL;
    s_audio_frame = 0;
    s_audio_visualizer_show = false;
    for (uint32_t i = 0; i < T113_AUDIO_BAR_COUNT; i++) {
        s_audio_bars[i] = NULL;
    }
    s_playlist_count = 0;

    if (s_ui != NULL && s_ui->screen.player_imgbtn_play != NULL) {
        lv_obj_add_flag(s_ui->screen.player_imgbtn_play, LV_OBJ_FLAG_CHECKABLE);
    }

    if (s_ui != NULL && s_ui->screen.TITLE != NULL) {
        lv_obj_set_style_text_outline_stroke_width(s_ui->screen.TITLE, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_ui->screen.TITLE, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (s_ui != NULL && s_ui->screen.REC_led != NULL) {
        lv_led_set_brightness(s_ui->screen.REC_led, 0);
    }

    if (s_ui != NULL && s_ui->screen.fps != NULL) {
        lv_label_set_text(s_ui->screen.fps, "FPS: 0");
    }

    if (s_ui != NULL && s_ui->screen.vedioplay != NULL) {
        s_video_message = lv_label_create(s_ui->screen.vedioplay);
        lv_label_set_text(s_video_message, "");
        lv_obj_center(s_video_message);
        lv_obj_set_style_text_color(s_video_message, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(s_video_message, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(s_video_message, &lv_font_LiberationSans_25,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);

        for (uint32_t i = 0; i < T113_AUDIO_BAR_COUNT; i++) {
            s_audio_bars[i] = lv_bar_create(s_ui->screen.vedioplay);
            lv_obj_set_size(s_audio_bars[i], 8, 118);
            lv_obj_set_x(s_audio_bars[i], (int32_t)i * 15 - 112);
            lv_obj_set_y(s_audio_bars[i], 0);
            lv_obj_set_align(s_audio_bars[i], LV_ALIGN_CENTER);
            lv_bar_set_range(s_audio_bars[i], 0, 100);
            lv_bar_set_value(s_audio_bars[i], 0, LV_ANIM_OFF);
            lv_obj_set_style_radius(s_audio_bars[i], 3, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(s_audio_bars[i], lv_color_hex(0xe7e7e7),
                                      LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(s_audio_bars[i], 180, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(s_audio_bars[i], 3, LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(s_audio_bars[i], lv_color_hex(0x12c94f),
                                      LV_PART_INDICATOR | LV_STATE_DEFAULT);
            lv_obj_add_flag(s_audio_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void t113_gui_set_playing(bool playing)
{
    if (s_ui == NULL || s_ui->screen.player_imgbtn_play == NULL) {
        return;
    }

    if (playing) {
        lv_imagebutton_set_src(s_ui->screen.player_imgbtn_play,
                               LV_IMAGEBUTTON_STATE_RELEASED,
                               NULL, &btn_pause_100x100_ARGB8888, NULL);
        lv_imagebutton_set_src(s_ui->screen.player_imgbtn_play,
                               LV_IMAGEBUTTON_STATE_PRESSED,
                               NULL, &btn_pause_100x100_ARGB8888, NULL);
        lv_obj_add_state(s_ui->screen.player_imgbtn_play, LV_STATE_CHECKED);
    } else {
        lv_imagebutton_set_src(s_ui->screen.player_imgbtn_play,
                               LV_IMAGEBUTTON_STATE_RELEASED,
                               NULL, &btn_play_100x100_ARGB8888, NULL);
        lv_imagebutton_set_src(s_ui->screen.player_imgbtn_play,
                               LV_IMAGEBUTTON_STATE_PRESSED,
                               NULL, &btn_play_100x100_ARGB8888, NULL);
        lv_obj_clear_state(s_ui->screen.player_imgbtn_play, LV_STATE_CHECKED);
    }
    lv_obj_invalidate(s_ui->screen.player_imgbtn_play);
}

void t113_gui_set_screen_title(const char *text)
{
    if (s_ui == NULL || s_ui->screen.TITLE == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(s_ui->screen.TITLE, text);
}

void t113_gui_set_progress(uint32_t value, uint32_t max)
{
    if (s_ui == NULL || s_ui->screen.player_slider_time == NULL || max == 0U) {
        return;
    }

    lv_slider_set_range(s_ui->screen.player_slider_time, 0, (int32_t)max);

    if (value > max) {
        value = max;
    }

    lv_slider_set_value(s_ui->screen.player_slider_time, (int32_t)value, LV_ANIM_OFF);
}

void t113_gui_set_title(const char *title, const char *subtitle)
{
    if (s_ui == NULL) {
        return;
    }

    if (title != NULL && s_ui->screen.container_title_label_title_music != NULL) {
        lv_label_set_text(s_ui->screen.container_title_label_title_music, title);
    }

    if (subtitle != NULL && s_ui->screen.container_title_label_title_author != NULL) {
        lv_label_set_text(s_ui->screen.container_title_label_title_author, subtitle);
    }
}

void t113_gui_set_usb_status(const char *text)
{
    if (s_ui == NULL || s_ui->screen.usbsta == NULL || text == NULL) {
        return;
    }

    lv_label_set_text(s_ui->screen.usbsta, text);
}

void t113_gui_set_fps(uint32_t fps)
{
    char text[32];

    if (s_ui == NULL || s_ui->screen.fps == NULL) {
        return;
    }

    lv_snprintf(text, sizeof(text), "FPS: %" LV_PRIu32, fps);
    lv_label_set_text(s_ui->screen.fps, text);
}

void t113_gui_set_rec_led(uint8_t brightness)
{
    if (s_ui == NULL || s_ui->screen.REC_led == NULL) {
        return;
    }

    lv_led_set_brightness(s_ui->screen.REC_led, brightness);
}

void t113_gui_set_video_message(const char *text)
{
    if (s_video_message == NULL) {
        return;
    }

    lv_label_set_text(s_video_message, text != NULL ? text : "");
    lv_obj_center(s_video_message);
}

void t113_gui_set_audio_visualizer(bool show)
{
    s_audio_visualizer_show = show;
    s_audio_frame = 0;
    for (uint32_t i = 0; i < T113_AUDIO_BAR_COUNT; i++) {
        if (s_audio_bars[i] == NULL) {
            continue;
        }
        if (show) {
            lv_obj_clear_flag(s_audio_bars[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_bar_set_value(s_audio_bars[i], 0, LV_ANIM_OFF);
            lv_obj_add_flag(s_audio_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_video_message != NULL && show) {
        lv_label_set_text(s_video_message, "");
    }
}

void t113_gui_audio_visualizer_tick(void)
{
    if (!s_audio_visualizer_show || T113_SPECTRUM_COUNT == 0U) {
        return;
    }
    if ((s_audio_frame % 4U) != 0U) {
        s_audio_frame++;
        return;
    }
    for (uint32_t i = 0; i < T113_AUDIO_BAR_COUNT; i++) {
        uint16_t value;
        uint32_t row;

        if (s_audio_bars[i] == NULL) {
            continue;
        }
        row = ((s_audio_frame / 4U) + i * 7U) % T113_SPECTRUM_COUNT;
        value = spectrum_2[row][i % 4U];
        if (value > 100U) {
            value = 100U;
        }
        lv_bar_set_value(s_audio_bars[i], value, LV_ANIM_OFF);
    }
    s_audio_frame++;
}

void t113_gui_playlist_clear(void)
{
    if (s_ui == NULL || s_ui->screen.playlist == NULL) {
        return;
    }

    lv_obj_clean(s_ui->screen.playlist);
    s_playlist_count = 0;
}

bool t113_gui_playlist_add(const char *text)
{
    lv_obj_t *button;
    lv_obj_t *label;

    if (s_ui == NULL || s_ui->screen.playlist == NULL || text == NULL) {
        return false;
    }
    if (s_playlist_count >= (uint32_t)(sizeof(s_playlist_buttons) / sizeof(s_playlist_buttons[0]))) {
        return false;
    }

    button = lv_list_add_button(s_ui->screen.playlist, NULL, text);
    if (button == NULL) {
        return false;
    }

    lv_obj_set_height(button, 34);
    lv_obj_set_style_text_font(button, &lv_font_LiberationSans_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(button, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    label = lv_obj_get_child(button, 0);
    if (label != NULL) {
        lv_obj_set_width(label, 96);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_font(label, &lv_font_LiberationSans_18,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(label, lv_color_hex(0x000000),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    s_playlist_buttons[s_playlist_count++] = button;
    return true;
}

void t113_gui_playlist_select(uint32_t index)
{
    uint32_t i;

    for (i = 0; i < s_playlist_count; i++) {
        lv_obj_t *button = s_playlist_buttons[i];

        if (button == NULL) {
            continue;
        }

        if (i == index) {
            lv_obj_t *label = lv_obj_get_child(button, 0);

            if (label != NULL) {
                lv_obj_set_width(label, 96);
                lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
            }
            lv_obj_set_style_bg_color(button, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(button, 150, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(button, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_scroll_to_view(button, LV_ANIM_OFF);
        } else {
            lv_obj_set_style_bg_opa(button, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_color(button, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

bool t113_gui_get_video_rect(t113_gui_rect_t *rect)
{
    lv_area_t area;

    if (rect == NULL || s_ui == NULL || s_ui->screen.vedioplay == NULL) {
        return false;
    }

    lv_obj_update_layout(s_ui->screen.screen);
    lv_obj_get_coords(s_ui->screen.vedioplay, &area);

    rect->x = area.x1;
    rect->y = area.y1;
    rect->w = area.x2 - area.x1 + 1;
    rect->h = area.y2 - area.y1 + 1;

    return true;
}

bool t113_gui_get_video_content_rect(t113_gui_rect_t *rect)
{
    const int border = 4;

    if (!t113_gui_get_video_rect(rect)) {
        return false;
    }

    rect->x += border;
    rect->y += border;
    rect->w -= border * 2;
    rect->h -= border * 2;

    return rect->w > 0 && rect->h > 0;
}

void lv_demo_music_resume(void)
{
    t113_gui_set_playing(true);
    t113_gui_on_play();
}

void lv_demo_music_pause(void)
{
    t113_gui_set_playing(false);
    t113_gui_on_pause();
}

void lv_demo_music_album_next(bool next)
{
    if (next) {
        t113_gui_on_next();
    } else {
        t113_gui_on_previous();
    }
}

void lv_demo_music_play(uint32_t id)
{
    (void)id;
    t113_gui_set_playing(true);
    t113_gui_on_play();
}

void lv_demo_music_list_btn_check(uint32_t track_id, bool state)
{
    (void)track_id;
    (void)state;
}
