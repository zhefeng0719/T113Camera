#ifndef INPUT_KEY_H
#define INPUT_KEY_H

enum app_key {
    APP_KEY_NONE = 0, /* 没有应用按键事件。 */
    APP_KEY_ENTER, /* Linux KEY_ENTER=28，对应左键选择上一项。 */
    APP_KEY_BACK, /* Linux KEY_BACK=158，中键短按播放或录制，长按切换模式。 */
    APP_KEY_NEXT, /* Linux KEY_NEXT=407，右键选择下一项。 */
};

enum app_key_action {
    APP_KEY_ACTION_NONE = 0, /* 无动作，轮询仍应继续。 */
    APP_KEY_ACTION_SHORT, /* 松开时产生，长按触发后不再产生。 */
    APP_KEY_ACTION_LONG, /* 按住满 1.5 秒产生，每次按压只发一次。 */
};

struct app_key_event {
    enum app_key key;
    enum app_key_action action;
};

struct input_key {
    int fd; /* 非阻塞 input event 句柄，关闭后为 -1。 */
    int pressed_enter; /* 左键是否保持按下。 */
    int pressed_back; /* 中键是否保持按下。 */
    int pressed_next; /* 右键是否保持按下。 */
    int long_sent_enter; /* 左键本次按压是否已经触发长按。 */
    int long_sent_back; /* 中键长按触发标记，防止松开又启动播放。 */
    int long_sent_next; /* 右键长按触发标记。 */
    unsigned long long press_ms_enter; /* 左键单调按下时刻，毫秒。 */
    unsigned long long press_ms_back; /* 中键单调按下时刻，毫秒。 */
    unsigned long long press_ms_next; /* 右键单调按下时刻，毫秒。 */
};

int input_key_open(struct input_key *input, const char *device);
int input_key_poll(struct input_key *input, struct app_key_event *event); /* 1 有事件、0 暂无、-1 读取异常；由应用分派业务动作。 */
void input_key_close(struct input_key *input);

#endif
