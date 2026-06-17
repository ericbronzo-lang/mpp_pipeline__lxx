# PROJECT_OVERVIEW

本文只概括 `big_core/mpp_pipeline` 目录下的 MPP 管线代码，不覆盖仓库里的其它示例、配置文件或历史文档。

文件命名前缀按任务目标划分：`media_*` 负责大核 MPP 底层媒体能力，`stream_*` 负责 H.265/NALU 码流出口和小核推流对接准备，`motion_*` 负责 AI 低清帧、运动检测算法契约和运动事件闭环。

## 1. 函数视角的文件抽象

### big_core/mpp_pipeline/mpp_pipeline.c

- `main(void) -> int`
  输入：无命令行参数；读取全局宏配置，如 `SENSOR_TYPE`、`ENC_WIDTH`、`ENC_HEIGHT`、`VENC_CHN`、`AUTO_EXIT_SEC`。
  输出：进程退出码；`0` 表示管线完成并清理成功，非 `0` 表示初始化或运行阶段失败。运行期间输出 MPP 初始化、码流、统计和清理日志。

- `sig_handler(signo: int) -> void`
  输入：系统信号编号，主要来自 `SIGINT` 或 `SIGTERM`。
  输出：无返回值；把全局 `g_running` 置为 `0`，触发主循环、码流线程和 AI 线程退出。

### big_core/mpp_pipeline/mpp_pipeline.h

- `pipeline_config_constants() -> compile_time_contract`
  输入：无运行期输入；由头文件宏给出固定配置。
  输出：编译期契约，包括编码尺寸 `1920x1080`、AI 旁路尺寸 `640x480`、VB 块数、VENC/VICAP 通道号、RT-Thread 线程栈和优先级等。

- `pipeline_public_api() -> function_declarations`
  输入：无运行期输入；头文件聚合各模块公开函数声明。
  输出：跨文件调用接口，例如 `vb_init()`、`venc_init()`、`vicap_config()`、`vi_bind_venc()`、`ai_motion_thread_start()`、`pipeline_cleanup()`。

### big_core/mpp_pipeline/mpp_types.h

- `AI_GRAY_MAX_WIDTH / AI_GRAY_MAX_HEIGHT -> compile_time_contract`
  输入：无运行期输入。
  输出：AI 灰度检测输入的最大尺寸，当前为 `640x480`。

- `mpp_stream_frame_desc(chn: k_u32, pts: k_u64, pack_cnt: k_u32, packs: mpp_stream_pack_desc[]) -> encoded_frame_descriptor`
  输入：VENC 输出的通道号、时间戳、pack 数量和每个 pack 的物理地址、长度、类型。
  输出：传给 `stream_export_submit()` 的统一码流帧描述。

- `ai_gray_frame_view(frame_id: k_u64, timestamp_ms: k_u64, width: k_u32, height: k_u32, stride: k_u32, y: const k_u8 *) -> ai_luma_view`
  输入：AI 通道 dump 出来的低清帧元数据和 Y 平面虚拟地址。
  输出：传给运动检测适配层的灰度图视图；`y` 是 NV12 的 Y 平面，可当灰度图使用；调用方必须按 `stride` 逐行访问，不拥有底层帧资源，资源由 `ai_frame_release()` 释放。

- `motion_event_msg(event_id: k_u32, detect_time_ms: k_u64, motion_score: k_u32, osd_duration_ms: k_u32, request_snapshot: k_u32) -> motion_event`
  输入：运动检测结果和事件生成时间。
  输出：AI 线程用于触发 OSD 或后续抓拍逻辑的事件消息。

### big_core/mpp_pipeline/media_vb.c

- `vb_init(void) -> k_s32`
  输入：无参数；使用 `INPUT_BUF_CNT`、`OUTPUT_BUF_CNT`、`AI_BUF_CNT`、`FRAME_BUF_SIZE`、`STREAM_BUF_SIZE`、`AI_CHN_BUF_SIZE` 等宏。
  输出：`0` 表示 VB 公共内存池配置并初始化成功；非 `0` 为 `kd_mpi_vb_set_config()` 或 `kd_mpi_vb_init()` 错误码。

  内存池含义：
  - Pool 0：主输入帧池，`INPUT_BUF_CNT` 块，每块 `FRAME_BUF_SIZE`，供 1080P 采集链路使用。
  - Pool 1：VENC 码流池，`OUTPUT_BUF_CNT` 块，每块 `STREAM_BUF_SIZE`，用于编码后的压缩码流缓存。
  - Pool 2：AI 低清旁路帧池，`AI_BUF_CNT` 块，每块 `AI_CHN_BUF_SIZE`，供 `640x480` AI 通道 dump 使用。

### big_core/mpp_pipeline/media_vicap.c

- `vicap_set_channel_attr(chn: k_vicap_chn, width: k_u32, height: k_u32, buffer_num: k_u32, buffer_size: k_u32, pix_format: k_pixel_format, use_sensor_crop_win: k_bool, sensor_win: const k_vicap_window *) -> k_s32`
  输入：VICAP 通道号、输出尺寸、buffer 数量和大小、像素格式、是否复用 sensor 裁剪窗口。
  输出：`0` 表示通道属性设置成功；非 `0` 为 `kd_mpi_vicap_set_chn_attr()` 错误码。该函数为文件内静态辅助函数。

- `vicap_try_config(sensor_type: k_vicap_sensor_type) -> k_s32`
  输入：一个具体的 sensor 类型，如 `GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR`。
  输出：`0` 表示查询 sensor、配置 VICAP device、配置主通道和 AI 通道、初始化 VICAP 全部成功；非 `0` 表示其中某一步失败。

- `vicap_config(sensor_type: k_vicap_sensor_type) -> k_s32`
  输入：首选 sensor 类型。
  输出：`0` 表示首选类型或 GC2093 CSI fallback 配置成功；非 `0` 表示全部尝试失败。

- `vicap_start(void) -> k_s32`
  输入：无参数；使用固定 `VICAP_DEV`。
  输出：`0` 表示 `kd_mpi_vicap_start_stream()` 成功，VICAP 开始出帧；非 `0` 为启动错误码。

### big_core/mpp_pipeline/media_venc.c

- `venc_init(chn: k_u32, bitrate: k_u32) -> k_s32`
  输入：VENC 通道号和目标码率，单位 kbps。
  输出：`0` 表示 H.265 CBR 编码通道创建并启动成功；非 `0` 为创建或启动错误码。

- `stream_thread(arg: void *) -> void`
  输入：RT-Thread 线程参数，转换后作为 VENC 通道号。
  输出：无函数返回值；循环从 VENC 获取 `k_venc_stream`，转换成 `mpp_stream_frame_desc`，提交给 `stream_export_submit()`，退出时释放 `g_stream_exit_sem`。

### big_core/mpp_pipeline/media_bind.c

- `vi_bind_venc(void) -> k_s32`
  输入：无参数；使用固定主链路 `VICAP_CHN` 和 `VENC_CHN`。
  输出：`0` 表示 `VI -> VENC` 硬件绑定成功；非 `0` 为 `kd_mpi_sys_bind()` 错误码。AI 通道不在这里绑定。

### big_core/mpp_pipeline/motion_ai_frame.c

- `ai_now_ms(void) -> k_u64`
  输入：无参数。
  输出：当前单调时钟毫秒值。该函数为文件内静态辅助函数。

- `ai_frame_stride(frame_info: const k_video_frame_info *) -> k_u32`
  输入：VICAP dump 出来的帧信息。
  输出：Y 平面 stride；如果 SDK 未填写 stride，则回退为帧宽。该函数为文件内静态辅助函数。

- `ai_frame_y_size(frame_info: const k_video_frame_info *) -> k_u32`
  输入：VICAP dump 出来的帧信息。
  输出：需要 mmap 的 Y 平面字节数，即 `stride * height`。该函数为文件内静态辅助函数。

- `ai_frame_channel_init(void) -> k_s32`
  输入：无参数；使用 `AI_VICAP_CHN`、`AI_WIDTH`、`AI_HEIGHT` 等宏。
  输出：`0` 表示 AI 帧通道逻辑初始化完成，并重置 AI 帧序号。

- `ai_frame_try_get(view: ai_gray_frame_view *, handle: void **) -> k_s32`
  输入：用于接收灰度视图的 `view`，以及用于接收资源句柄的 `handle`。
  输出：`0` 表示从 AI VICAP 通道 dump 到一帧、mmap Y 平面并填好 `view`；非 `0` 表示超时、无帧、分配失败或 mmap 失败。

- `ai_frame_release(handle: void *) -> k_s32`
  输入：`ai_frame_try_get()` 返回的资源句柄。
  输出：`0` 表示已 munmap 并释放 dump frame；非 `0` 表示释放过程中的 SDK 错误码。

- `ai_frame_channel_deinit(void) -> void`
  输入：无参数。
  输出：无返回值；输出 AI 帧通道反初始化日志。

### big_core/mpp_pipeline/motion_adapter.c

- `motion_now_ms(void) -> k_u64`
  输入：无参数。
  输出：当前单调时钟毫秒值。该函数为文件内静态辅助函数。

- `motion_adapter_init(void) -> k_s32`
  输入：无参数。
  输出：`0` 表示运动适配层初始化完成，并重置事件序号。

- `motion_adapter_process(frame: const ai_gray_frame_view *, event: motion_event_msg *, has_event: k_bool *) -> k_s32`
  输入：AI 灰度帧、事件输出结构体、是否产生事件的输出标志。
  输出：`0` 表示处理成功；当检测到运动时填充 `event` 并把 `has_event` 置为 `K_TRUE`。

- `motion_adapter_deinit(void) -> void`
  输入：无参数。
  输出：无返回值；输出运动适配层反初始化日志。

### big_core/mpp_pipeline/motion_detect.h

- `motion_detect_process(frame: const ai_gray_frame_view *, result: motion_detect_result *) -> k_s32`
  输入：一帧 AI 灰度图视图和用于接收检测结果的结构体。
  输出：`0` 表示算法处理成功；B 题算法可提供同名强符号覆盖默认弱符号实现。B 侧只填写检测结果，不直接操作 OSD、VICAP、VENC、线程或帧释放。

### big_core/mpp_pipeline/motion_detect_default.c

- `motion_detect_process(frame: const ai_gray_frame_view *, result: motion_detect_result *) -> k_s32`
  输入：AI 低清通道 NV12 的 Y 平面视图；逐行读取时使用 `stride`，不假设 `stride == width`。
  输出：默认弱符号帧差示例结果；首帧建立基线，后续根据变化像素比例输出 `motion_score` 并在达到阈值时置 `is_motion = 1`。

### big_core/mpp_pipeline/motion_thread.c

- `ai_motion_thread(arg: void *) -> void`
  输入：RT-Thread 线程参数，当前未使用。
  输出：无函数返回值；循环获取 AI 帧、调用运动检测适配层、按事件触发 OSD、释放 AI 帧，退出时释放 `g_ai_motion_exit_sem`。该函数为文件内静态线程入口。

- `ai_motion_thread_start(void) -> k_s32`
  输入：无参数。
  输出：`0` 表示 AI 帧通道、运动适配层、退出信号量和 RT-Thread 线程全部启动成功；非 `0` 表示对应初始化或线程创建失败。

- `ai_motion_thread_stop(void) -> void`
  输入：无参数。
  输出：无返回值；停止 AI 线程，等待其释放退出信号量，并反初始化运动适配层和 AI 帧通道。

### big_core/mpp_pipeline/stream_export.c

- `stream_export_init(mode: stream_export_mode) -> k_s32`
  输入：码流导出模式；当前有效模式是 `STREAM_EXPORT_LOCAL_LOG`。
  输出：`0` 表示导出模块初始化成功，并重置导出帧计数。

- `stream_export_submit(frame: const mpp_stream_frame_desc *) -> k_s32`
  输入：由 `stream_thread()` 组装的码流帧描述，包含 pack 数量、长度和类型。
  输出：`0` 表示处理成功；本地日志模式下打印 `Get NALU, Size: ...` 或 SPS/PPS header 信息。

- `stream_export_deinit(void) -> void`
  输入：无参数。
  输出：无返回值；若导出模块已初始化，则输出反初始化日志并清除状态。

### big_core/mpp_pipeline/media_osd.c

- `osd_init(void) -> k_s32`
  输入：无参数。
  输出：`0` 表示 OSD stub 初始化完成；当前尚未接入真实 VENC 2D OSD MMZ buffer。

- `osd_set_motion_visible(visible: k_u32, duration_ms: k_u32) -> k_s32`
  输入：是否显示运动提示的标志，以及持续时间毫秒数。
  输出：`0` 表示调用成功；当前为 stub，仅打印日志。

- `osd_deinit(void) -> void`
  输入：无参数。
  输出：无返回值；若 OSD stub 已初始化，则输出反初始化日志并清除状态。

### big_core/mpp_pipeline/media_cleanup.c

- `pipeline_cleanup(void) -> void`
  输入：无参数；读取全局 `g_status` 判断已经初始化到哪一步。
  输出：无返回值；按逆序停止 AI 线程、关闭导出和 OSD、停止 VICAP、解绑 VI/VENC、反初始化 VICAP、停止并销毁 VENC、关闭 VENC fd、退出 VB。
