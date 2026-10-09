# T113Camera

T113Camera 是基于全志 T113S3 开发板的嵌入式相机和媒体播放器原型。项目面向 5 寸 800x480 屏幕、UVC USB 摄像头、板载音频、U 盘存储和三按键交互，核心目标是完成一个可录制、可播放、可扩展的嵌入式音视频应用。

## 功能概览

支持 MP3/AAC/WAV 等音频播放，支持 MP4/AVI 等容器解析，支持 H264/MJPEG 硬件解码显示，支持 JPG 图片预览，支持 UVC USB 摄像头实时预览，支持 MJPEG 视频录制并封装为 AVI，支持板载音频采集与播放，支持 U 盘媒体扫描与保存，支持 LVGL 图形界面和三按键交互，支持 G2D 硬件缩放与格式转换。

## 当前能力

- 播放模式扫描 U 盘媒体目录并显示播放列表。
- 录制模式使用 USB UVC 摄像头采集 MJPEG，并实时显示到 UI 视频区域。
- 录制文件以 `record1.avi` 到 `record5.avi` 保存到 U 盘。
- AVI 录制采用 MJPEG 视频流和 PCM S16_LE 音频流。
- MP4 播放支持 H264/H265 视频流，FFmpeg 负责解封装，CedarC 负责硬解码。
- MP3/AAC 使用 FFmpeg 解码为 PCM 后交给 ALSA 播放。
- WAV 使用 PCM WAV 播放链路。
- JPG/JPEG 可在视频区域预览。
- 音频播放时，视频区域显示绿色音量条。
- 三个实体按键用于选择、确认、返回和模式切换。

## 技术栈

- SoC：Allwinner T113S3
- 系统：Tina / Buildroot Linux
- 语言：C
- UI：LVGL 9 + NXP GUI Guider
- 显示：`/dev/disp` 图层
- 视频采集：V4L2 / UVC / MJPEG
- 视频解码：CedarC 硬件解码
- 图像处理：G2D 缩放和 YUV 到 ARGB8888 转换
- 音频：ALSA `hw:0,0`
- 容器和音频编解码：FFmpeg API
- 存储：U 盘 `/mnt/usb/sda/T113Camera`

## 目录结构

```text
src/
  main.c                    程序入口、模式切换、按键调度和主流程

modules/audio/
  audio_alsa.*              ALSA 播放/采集基础封装
  audio_capture.*           录制音频采集线程
  ffmpeg_audio_player.*     MP3/AAC 等音频解码播放
  wav_player.*              WAV 播放

modules/video/
  video_capture.*           V4L2 UVC 摄像头采集
  image_convert.*           CedarC 解码封装

modules/display/
  disp_layer_display.*      /dev/disp 图层显示
  g2d_display.*             G2D 缩放和格式转换
  framebuffer_display.*     framebuffer fallback

modules/storage/
  ffmpeg_mjpeg_muxer.*      AVI 封装
  ffmpeg_mjpeg_demuxer.*    AVI/MP4/MKV 解封装视频 packet
  ffmpeg_pcm_demuxer.*      PCM 音频解封装
  mjpeg_save.*              MJPEG 裸流保存工具

modules/media/
  media_list.*              U 盘媒体扫描和文件类型识别

modules/input/
  input_key.*               三按键输入

modules/ui_lvgl/
  t113_ui_lvgl.*            LVGL 到 disp 图层的适配

ui/
  GUI Guider 生成代码和 LVGL 依赖
```

## 媒体目录

板端 U 盘目录约定如下：

```text
/mnt/usb/sda/T113Camera/
  AUDIO/                    MP3 / AAC / WAV
  DCIM/                     JPG / JPEG
  VIDEO/                    AVI / MP4 / MJPEG
```

`Dat/` 目录只作为本地测试素材暂存目录，不建议提交到 GitHub。

## 数据链路

录制链路：

```text
UVC 摄像头
-> V4L2 MJPEG
-> CedarC 解码
-> G2D 缩放/转换
-> /dev/disp 显示

MJPEG packet
-> FFmpeg AVI muxer
-> U 盘 VIDEO/recordN.avi

板载麦克风
-> ALSA PCM S16_LE
-> FFmpeg AVI muxer
```

播放链路：

```text
U 盘媒体文件
-> FFmpeg demux
-> MJPEG/H264/H265 packet
-> CedarC 硬解
-> G2D 缩放/转换
-> /dev/disp 显示

MP3/AAC/WAV
-> FFmpeg 或 WAV parser
-> PCM S16_LE
-> ALSA 播放
```

## 按键交互

播放模式：

```text
NEXT  短按：下一项
ENTER 短按：上一项
BACK  短按：播放/暂停
BACK  长按：进入录制模式
```

录制模式：

```text
BACK  长按：返回播放模式
BACK  短按：开始/停止录制
NEXT  短按：选择下一个录制文件名
ENTER 短按：选择上一个录制文件名
```

## 编译

在 Ubuntu 虚拟机中编译：

```sh
cd /mnt/hgfs/QZ-T113S/Project/T113Camera
make
```

输出文件：

```text
output/t113_camera
```

## 上传和运行

从 Windows 上传到板卡：

```powershell
scp E:\QZ-T113S\Project\T113Camera\output\t113_camera root@192.168.1.77:/root/Project/T113Camera/t113_camera
```

在板卡运行：

```sh
cd /root/Project/T113Camera
chmod +x t113_camera
./t113_camera
```

指定文件播放测试：

```sh
./t113_camera ui /mnt/usb/sda/T113Camera/VIDEO/guimie.mp4
./t113_camera ui /mnt/usb/sda/T113Camera/AUDIO/Abdulla.mp3
./t113_camera ui /mnt/usb/sda/T113Camera/DCIM/yln.jpg
```

命令行录制测试：

```sh
./t113_camera 5
```

## 当前限制

- 容器视频首帧自动预览已暂时关闭，避免刚录制完成后切换 AVI 时触发 CedarC/G2D/disp 资源竞争。
- 录制采用 MJPEG + PCM AVI，文件体积较大，但链路简单、适合当前硬件。
- MP4/H264 播放已接入硬解，但帧率调度仍需继续优化为读取真实容器帧率。
- 麦克风录音质量仍有优化空间，包括增益、底噪和滤波。
- 当前交互基于三按键，暂不使用触摸。

## 后续计划

- 将播放、录制、UI 刷新整理成更清晰的状态机。
- 为 AVI/MP4 首帧预览设计异步、可取消、限频版本。
- 增加录制剩余空间检测和文件名自动递增。
- 完善音视频同步和真实帧率调度。
- 增加拍照模式，保存 JPG 到 `DCIM/`。
- 增加音量控制、录音增益配置和简单降噪。
- 增加开机自启动脚本和发布版镜像配置。
