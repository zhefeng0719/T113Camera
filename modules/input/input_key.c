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

#define APP_KEY_LONG_PRESS_MS 1500ULL /* 长按阈值 1.5 秒，单调时间判断，不依赖驱动重复事件频率。 */

static unsigned long long monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL +
           (unsigned long long)ts.tv_nsec / 1000000ULL;
}

static enum app_key map_linux_key(unsigned short code) /* 内核 key code 转应用枚举；物理按键编号不等于 key code。 */
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

static int *pressed_flag_for_key(struct input_key *input, enum app_key key) /* 选择对应按键的按下状态，状态只由轮询线程维护。 */
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

static int *long_flag_for_key(struct input_key *input, enum app_key key) /* 记录本次按住是否已触发长按，避免重复触发。 */
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

static unsigned long long *press_ms_for_key(struct input_key *input, enum app_key key) /* 保存各键按下时刻，以计算持续按住的时间。 */
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
    if (!*pressed || *long_sent) /* 未按住或已触发过长按时，不再产生长按事件。 */
        return 0;
    if (monotonic_ms() - *press_ms < APP_KEY_LONG_PRESS_MS) /* 未达到阈值继续等待，无需驱动发送 value=2。 */
        return 0;

    *long_sent = 1; /* 本次按压只允许产生一个长按。 */
    event->key = key;
    event->action = APP_KEY_ACTION_LONG; /* 交给应用判断模式切换，输入层不调用 UI 或媒体模块。 */
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
    input->fd = open(device, O_RDONLY | O_NONBLOCK); /* 非阻塞读取，主循环还可以刷新界面和监测存储。 */
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
        event->key = APP_KEY_NONE; /* 每次轮询先清空输出，避免复用旧事件。 */
        event->action = APP_KEY_ACTION_NONE;
    }
    if (input == NULL || input->fd < 0 || event == NULL)
        return -1;

    if (check_long_press(input, event)) /* 读取新事件前检查保持按住的键，支持无自动重复的驱动。 */
        return 1;

    while ((n = read(input->fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) { /* 按完整 input_event 读取，过滤同步事件与无关按键。 */
        enum app_key key;
        int *pressed;
        int *long_sent;
        unsigned long long *press_ms;

        if (ev.type != EV_KEY) /* 忽略 EV_SYN、坐标等非按键事件。 */
            continue;

        key = map_linux_key(ev.code);
        if (key == APP_KEY_NONE)
            continue;

        pressed = pressed_flag_for_key(input, key);
        long_sent = long_flag_for_key(input, key);
        press_ms = press_ms_for_key(input, key);
        if (pressed == NULL || long_sent == NULL || press_ms == NULL)
            continue;

        if (ev.value == 1) { /* 按下只记录时间，短按在松开时判定。 */
            *pressed = 1;
            *long_sent = 0; /* 本次按压结束，为下一次按压复位。 */
            *press_ms = monotonic_ms(); /* 用用户态单调时间记录按下时刻。 */
            continue;
        }
        if (ev.value == 2) { /* 驱动自动重复事件仅用于检测长按，不当作再次短按。 */
            if (check_long_press_one(input, key, event))
                return 1;
            continue;
        }
        if (ev.value == 0) { /* 松开时判定短按；已长按的按压不能再产生短按。 */
            *pressed = 0;
            if (!*long_sent) { /* 长按和短按互斥，避免模式切换后又误启动播放。 */
                event->key = key;
                event->action = APP_KEY_ACTION_SHORT; /* 返回一个短按动作给应用控制逻辑。 */
                return 1;
            }
            *long_sent = 0; /* 本次按压结束，为下一次按压复位。 */
            *press_ms = 0; /* 清除已结束的按压时间。 */
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
    input->fd = -1; /* 关闭后用 -1 标记无效句柄。 */
}
