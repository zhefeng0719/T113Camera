#!/bin/sh
# 已解压源码即可重建最小静态库；不下载、不改 Tina SDK 的系统配置。
set -eu
SDK=${SDK:-/home/zqboard/TinaSDK5}
SOURCE=${SOURCE:-/home/zqboard/thirdparty/src/ffmpeg-4.4.4}
PREFIX=${FFMPEG_ROOT:-/home/zqboard/thirdparty/ffmpeg-4.4-t113}
SYSROOT=${SYSROOT:-$SDK/out/t113/zqboard/buildroot/buildroot/host/arm-buildroot-linux-gnueabi/sysroot}
CROSS=${CROSS_COMPILE:-$SDK/out/toolchain/gcc-linaro-5.3.1-2016.05-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi-}
test -f "$SOURCE/configure"
test -x "${CROSS}gcc"
test -d "$SYSROOT"
cd "$SOURCE"
./configure --prefix="$PREFIX" --arch=arm --target-os=linux \
    --enable-cross-compile --cross-prefix="$CROSS" --sysroot="$SYSROOT" \
    --disable-everything --disable-programs --disable-doc --disable-debug \
    --disable-network --disable-avdevice --disable-avfilter --disable-swscale \
    --disable-postproc --disable-bzlib --enable-static --disable-shared \
    --enable-avformat --enable-avcodec --enable-avutil --enable-swresample \
    --enable-protocol=file --enable-muxer=avi \
    --enable-demuxer=avi,matroska,mjpeg,mov,mp3,aac,wav \
    --enable-parser=mjpeg,h264,hevc,aac,mpegaudio \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb \
    --enable-decoder=aac,mp3,mp3float,pcm_s16le
make -j"${JOBS:-4}"
make install
