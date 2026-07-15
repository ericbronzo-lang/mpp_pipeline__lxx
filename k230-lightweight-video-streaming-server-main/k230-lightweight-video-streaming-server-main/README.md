# K230 Lightweight Video Streaming Server

基于 K230 平台和 RT-Thread Smart 的轻量级视频流媒体服务器，实现 1080P H.265 编码、RTSP/RTP 推流、DATAFIFO 跨核 IPC、AI 运动检测、OSD 事件提示和 SD 卡快照保存。

## Features

- 1080P@15fps H.265 video capture and encoding
- Lightweight RTSP/RTP streaming
- DATAFIFO-based cross-core IPC
- AI motion detection on low-resolution side stream
- OSD event overlay for motion events
- SD card snapshot saving
- READ_DONE-based VENC buffer lifecycle management
