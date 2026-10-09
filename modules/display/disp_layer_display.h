#ifndef DISP_LAYER_DISPLAY_H
#define DISP_LAYER_DISPLAY_H

/* /dev/disp 图层显示上下文。 */
struct disp_layer_display {
    int fd;                         /* /dev/disp 文件描述符。 */
    unsigned int screen_width;      /* 屏幕显示宽度。 */
    unsigned int screen_height;     /* 屏幕显示高度。 */
    unsigned int channel;           /* disp 通道号，LCD 通常使用 0。 */
    unsigned int layer_id;          /* 图层号，当前使用 0。 */
    unsigned int zorder;            /* 图层叠放顺序，数值越大越靠上。 */
    int window_x;                   /* 图层显示窗口左上角 x。 */
    int window_y;                   /* 图层显示窗口左上角 y。 */
    unsigned int window_width;      /* 图层显示窗口宽度。 */
    unsigned int window_height;     /* 图层显示窗口高度。 */
};

/* 打开 /dev/disp 并准备一个显示图层。 */
int disp_layer_display_open(struct disp_layer_display *disp,
                            unsigned int screen_width,
                            unsigned int screen_height);
/* 打开 /dev/disp，并指定 channel/layer/zorder/window，用于 UI+视频双图层。 */
int disp_layer_display_open_ex(struct disp_layer_display *disp,
                               unsigned int screen_width,
                               unsigned int screen_height,
                               unsigned int channel,
                               unsigned int layer_id,
                               unsigned int zorder,
                               int window_x,
                               int window_y,
                               unsigned int window_width,
                               unsigned int window_height);
/* 把一个 ARGB8888 dmabuf 显示到 LCD 图层。 */
int disp_layer_display_show_argb8888(struct disp_layer_display *disp,
                                     int dmabuf_fd,
                                     unsigned int width,
                                     unsigned int height);
/* 只隐藏图层，不关闭 /dev/disp fd。 */
void disp_layer_display_hide(struct disp_layer_display *disp);
/* 关闭图层并释放 /dev/disp。 */
void disp_layer_display_close(struct disp_layer_display *disp);

#endif
