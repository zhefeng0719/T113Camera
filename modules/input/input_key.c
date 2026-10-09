#include "input_key.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef KEY_ENTER
#define KEY_ENTER 28
#endif

#ifndef KEY_BACK
#define KEY_BACK 158
#endif

#ifndef KEY_NEXT
#define KEY_NEXT 407
#endif

#define APP_KEY_LONG_PRESS_MS 1500ULL

static unsigned long long monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static enum app_key map_linux_key(unsigned short code)
{
    switch (code) {
    case KEY_ENTER:
        return APP_KEY_ENTER;
    case KEY_BACK:
        return APP_KEY_BACK;
    case KEY_NEXT:
        return APP_KEY_NEXT;
    default:
        return APP_KEY_NONE;
    }
}

static int *pressed_flag_for_key(struct input_key *input, enum app_key key)
{
    switch (key) {
    case APP_KEY_ENTER:
        return &input->pressed_enter;
    case APP_KEY_BACK:
        return &input->pressed_back;
    case APP_KEY_NEXT:
        return &input->pressed_next;
    default:
        return NULL;
    }
}

static int *long_flag_for_key(struct input_key *input, enum app_key key)
{
    switch (key) {
    case APP_KEY_ENTER:
        return &input->long_sent_enter;
    case APP_KEY_BACK:
        return &input->long_sent_back;
    case APP_KEY_NEXT:
        return &input->long_sent_next;
    default:
        return NULL;
    }
}

static unsigned long long *press_ms_for_key(struct input_key *input, enum app_key key)
{
    switch (key) {
    case APP_KEY_ENTER:
        return &input->press_ms_enter;
    case APP_KEY_BACK:
        return &input->press_ms_back;
    case APP_KEY_NEXT:
        return &input->press_ms_next;
    default:
        return NULL;
    }
}

static int check_long_press_one(struct input_key *input,
                                enum app_key key,
                                struct app_key_event *event)
{
    int *pressed = pressed_flag_for_key(input, key);
    int *long_sent = long_flag_for_key(input, key);
    unsigned long long *press_ms = press_ms_for_key(input, key);

    if (pressed == NULL || long_sent == NULL || press_ms == NULL)
        return 0;
    if (!*pressed || *long_sent)
        return 0;
    if (monotonic_ms() - *press_ms < APP_KEY_LONG_PRESS_MS)
        return 0;

    *long_sent = 1;
    event->key = key;
    event->action = APP_KEY_ACTION_LONG;
    return 1;
}

static int check_long_press(struct input_key *input, struct app_key_event *event)
{
    if (check_long_press_one(input, APP_KEY_ENTER, event))
        return 1;
    if (check_long_press_one(input, APP_KEY_BACK, event))
        return 1;
    if (check_long_press_one(input, APP_KEY_NEXT, event))
        return 1;
    return 0;
}

int input_key_open(struct input_key *input, const char *device)
{
    if (input == NULL || device == NULL)
        return -1;

    memset(input, 0, sizeof(*input));
    input->fd = open(device, O_RDONLY | O_NONBLOCK);
    if (input->fd < 0) {
        perror("open input");
        return -1;
    }

    return 0;
}

int input_key_poll(struct input_key *input, struct app_key_event *event)
{
    struct input_event ev;
    ssize_t n;

    if (event != NULL) {
        event->key = APP_KEY_NONE;
        event->action = APP_KEY_ACTION_NONE;
    }
    if (input == NULL || input->fd < 0 || event == NULL)
        return -1;

    if (check_long_press(input, event))
        return 1;

    while ((n = read(input->fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        enum app_key key;
        int *pressed;
        int *long_sent;
        unsigned long long *press_ms;

        if (ev.type != EV_KEY)
            continue;

        key = map_linux_key(ev.code);
        if (key == APP_KEY_NONE)
            continue;

        pressed = pressed_flag_for_key(input, key);
        long_sent = long_flag_for_key(input, key);
        press_ms = press_ms_for_key(input, key);
        if (pressed == NULL || long_sent == NULL || press_ms == NULL)
            continue;

        if (ev.value == 1) {
            *pressed = 1;
            *long_sent = 0;
            *press_ms = monotonic_ms();
            continue;
        }
        if (ev.value == 2) {
            if (check_long_press_one(input, key, event))
                return 1;
            continue;
        }
        if (ev.value == 0) {
            *pressed = 0;
            if (!*long_sent) {
                event->key = key;
                event->action = APP_KEY_ACTION_SHORT;
                return 1;
            }
            *long_sent = 0;
            *press_ms = 0;
        }
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("read input");
        return -1;
    }

    return 0;
}

void input_key_close(struct input_key *input)
{
    if (input == NULL)
        return;

    if (input->fd >= 0)
        close(input->fd);
    memset(input, 0, sizeof(*input));
    input->fd = -1;
}
