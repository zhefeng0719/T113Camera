/*
* Copyright 2026 NXP
* NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
* accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
* activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
* comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
* terms, then you may not retain, install, activate or otherwise use the software.
*/

#ifndef CUSTOM_H
#define CUSTOM_H
#ifdef __cplusplus
extern "C" {
#endif

#include "../generated/gui_guider.h"

void custom_init(gg_ui_t *ui);
void lv_demo_music_resume(void);
void lv_demo_music_pause(void);
void lv_demo_music_album_next(bool next);
void lv_demo_music_play(uint32_t id);
void lv_demo_music_list_btn_check(uint32_t track_id, bool state);

typedef struct {
    int x;
    int y;
    int w;
    int h;
} t113_gui_rect_t;

void t113_gui_set_playing(bool playing);
void t113_gui_set_screen_title(const char *text);
void t113_gui_set_progress(uint32_t value, uint32_t max);
void t113_gui_set_title(const char *title, const char *subtitle);
void t113_gui_set_usb_status(const char *text);
void t113_gui_set_fps(uint32_t fps);
void t113_gui_set_rec_led(uint8_t brightness);
void t113_gui_set_video_message(const char *text);
void t113_gui_set_audio_visualizer(bool show);
void t113_gui_audio_visualizer_tick(void);
void t113_gui_playlist_clear(void);
bool t113_gui_playlist_add(const char *text);
void t113_gui_playlist_select(uint32_t index);
bool t113_gui_get_video_rect(t113_gui_rect_t *rect);
bool t113_gui_get_video_content_rect(t113_gui_rect_t *rect);

void t113_gui_on_play(void);
void t113_gui_on_pause(void);
void t113_gui_on_previous(void);
void t113_gui_on_next(void);

#ifdef __cplusplus
}
#endif
#endif /* CUSTOM_H */
