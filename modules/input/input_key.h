#ifndef INPUT_KEY_H
#define INPUT_KEY_H

enum app_key {
    APP_KEY_NONE = 0,
    APP_KEY_ENTER,
    APP_KEY_BACK,
    APP_KEY_NEXT,
};

enum app_key_action {
    APP_KEY_ACTION_NONE = 0,
    APP_KEY_ACTION_SHORT,
    APP_KEY_ACTION_LONG,
};

struct app_key_event {
    enum app_key key;
    enum app_key_action action;
};

struct input_key {
    int fd;
    int pressed_enter;
    int pressed_back;
    int pressed_next;
    int long_sent_enter;
    int long_sent_back;
    int long_sent_next;
    unsigned long long press_ms_enter;
    unsigned long long press_ms_back;
    unsigned long long press_ms_next;
};

int input_key_open(struct input_key *input, const char *device);
int input_key_poll(struct input_key *input, struct app_key_event *event);
void input_key_close(struct input_key *input);

#endif
