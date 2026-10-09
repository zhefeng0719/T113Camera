SDK ?= /home/zqboard/TinaSDK5
SYSROOT ?= $(SDK)/out/t113/zqboard/buildroot/buildroot/host/arm-buildroot-linux-gnueabi/sysroot
CROSS_COMPILE ?= $(SDK)/out/toolchain/gcc-linaro-5.3.1-2016.05-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi-

CC := $(CROSS_COMPILE)gcc

CEDARC := $(SDK)/platform/allwinner/multimedia/libcedarc
LVGL_ROOT := ui/platform/lvgl
FFMPEG_ROOT ?= /home/zqboard/thirdparty/ffmpeg-4.4-t113

CFLAGS += -Wall -Wextra -O2
CFLAGS += --sysroot=$(SYSROOT)
CFLAGS += -I$(CEDARC)/include
CFLAGS += -I$(CEDARC)/vdecoder/include
CFLAGS += -I$(SDK)/kernel/linux-5.4/include/uapi/video
CFLAGS += -Imodules/audio
CFLAGS += -Imodules/common
CFLAGS += -Imodules/display
CFLAGS += -Imodules/input
CFLAGS += -Imodules/media
CFLAGS += -Imodules/storage
CFLAGS += -Imodules/ui_lvgl
CFLAGS += -Imodules/video
CFLAGS += -I$(FFMPEG_ROOT)/include
CFLAGS += -Iui/platform/simulator
CFLAGS += -Iui/platform/lvgl
CFLAGS += -Iui/platform/lvgl/src
CFLAGS += -Iui/generated
CFLAGS += -Iui/custom
CFLAGS += -DLV_CONF_INCLUDE_SIMPLE
CFLAGS += -D_GNU_SOURCE -DTINA_LINUX_SUPPORT=1

LDFLAGS += --sysroot=$(SYSROOT)
LDLIBS += -L$(SYSROOT)/usr/lib -lvdecoder -lvideoengine -lMemAdapter -lVE -lawion -lasound
LDLIBS += -L$(FFMPEG_ROOT)/lib -lavformat -lavcodec -lswresample -lavutil -lz -latomic
LDLIBS += -lpthread -lm

TARGET := output/t113_camera
OUT_DIR := output
OBJ_DIR := $(OUT_DIR)/obj

GUI_SRCS := $(shell find ui/generated ui/custom -name '*.c')
LVGL_SRCS := $(shell find $(LVGL_ROOT)/src \
	-path '$(LVGL_ROOT)/src/drivers' -prune -o \
	-path '$(LVGL_ROOT)/src/libs' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/dma2d' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/eve' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/espressif' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/nema_gfx' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/nxp' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/opengles' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/renesas' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/sdl' -prune -o \
	-path '$(LVGL_ROOT)/src/draw/vg_lite' -prune -o \
	-path '$(LVGL_ROOT)/src/others' -prune -o \
	-name '*.c' -print)
LVGL_SRCS += $(LVGL_ROOT)/src/libs/bin_decoder/lv_bin_decoder.c
LVGL_SRCS += $(shell find $(LVGL_ROOT)/src/others/observer -name '*.c')
LVGL_SRCS += $(shell find $(LVGL_ROOT)/src/others/sysmon -name '*.c')

SRCS := \
	src/main.c \
	modules/audio/audio_alsa.c \
	modules/audio/audio_capture.c \
	modules/audio/ffmpeg_audio_player.c \
	modules/audio/wav_player.c \
	modules/common/frame_queue.c \
	modules/common/playback_queue.c \
	modules/input/input_key.c \
	modules/media/media_list.c \
	modules/video/video_capture.c \
	modules/video/image_convert.c \
	modules/storage/mjpeg_save.c \
	modules/storage/ffmpeg_pcm_demuxer.c \
	modules/storage/ffmpeg_mjpeg_demuxer.c \
	modules/storage/ffmpeg_mjpeg_muxer.c \
	modules/display/framebuffer_display.c \
	modules/display/g2d_display.c \
	modules/display/disp_layer_display.c \
	modules/ui_lvgl/t113_ui_lvgl.c \
	$(GUI_SRCS) \
	$(LVGL_SRCS)

OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT_DIR)
