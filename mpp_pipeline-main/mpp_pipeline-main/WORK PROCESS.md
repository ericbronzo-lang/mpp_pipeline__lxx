## 1. 函数协作关系图

主初始化链路：

`main() -> vb_init() -> venc_init(VENC_CHN, ENC_BITRATE) -> osd_init() -> vicap_config(SENSOR_TYPE) -> vicap_try_config(sensor_type) -> vicap_set_channel_attr(VICAP_CHN, ...) -> vicap_set_channel_attr(AI_VICAP_CHN, ...) -> vi_bind_venc() -> vicap_start() -> ai_motion_thread_start() -> stream_export_init(STREAM_EXPORT_LOCAL_LOG) -> rt_thread_create(stream_thread)`

关键数据传递：
- `main()` 把编译期宏配置作为隐式输入，按顺序初始化硬件资源。
- `vb_init()` 创建三类 VB 内存：主输入帧、VENC 压缩码流、AI 低清帧。
- `venc_init()` 使用 `VENC_CHN` 和 `ENC_BITRATE` 创建 H.265 CBR 编码通道。
- `vicap_config()` 尝试 sensor 类型，并在失败时尝试 GC2093 其它 CSI fallback。
- `vicap_try_config()` 查询 sensor 信息后配置两个输出通道：主通道 `1920x1080 NV12`，AI 通道 `640x480`。
- `vi_bind_venc()` 只把主 VICAP 通道绑定到 VENC，形成硬件零拷贝编码路径。
- `ai_motion_thread_start()` 另起 RT-Thread 线程读取 AI 通道，和主编码链路并行。
- `stream_thread()` 由 RT-Thread 调度，从 VENC 取编码后的 NALU。

主码流运行链路：

`stream_thread(arg) -> kd_mpi_venc_query_status() -> kd_mpi_venc_get_stream() -> mpp_stream_frame_desc -> stream_export_submit(frame) -> kd_mpi_venc_release_stream()`

关键数据传递：
- `arg` 被转换为 VENC 通道号。
- `kd_mpi_venc_query_status()` 返回当前可取的 pack 数量。
- `kd_mpi_venc_get_stream()` 返回 `k_venc_stream`，其中每个 `k_venc_pack` 代表 header 或一段编码码流。
- `stream_thread()` 把 SDK 结构转换为项目内部的 `mpp_stream_frame_desc`。
- `stream_export_submit()` 按 pack 类型打印 NALU 大小，并维护本地导出帧计数。
- `kd_mpi_venc_release_stream()` 把码流资源归还给 VENC。

AI 低清旁路链路：

`ai_motion_thread_start() -> ai_frame_channel_init() -> motion_adapter_init() -> rt_thread_create(ai_motion_thread)`

`ai_motion_thread() -> ai_frame_try_get(view, handle) -> motion_adapter_process(view, event, has_event) -> motion_detect_process(view, result) -> osd_set_motion_visible(1, event.osd_duration_ms) -> ai_frame_release(handle)`

关键数据传递：
- `ai_frame_try_get()` 从 `AI_VICAP_CHN` dump 一帧低清图像，并 mmap Y 平面。
- `ai_gray_frame_view` 只描述灰度图视图，包含帧号、时间戳、宽高、stride、Y 指针。
- `motion_adapter_process()` 调用可替换的 `motion_detect_process()`。
- 默认弱符号 `motion_detect_process()` 不产生运动事件；若后续算法覆盖它，运动结果会被包装成 `motion_event_msg`。
- 当 `has_event == K_TRUE` 时，AI 线程调用 `osd_set_motion_visible()`。
- 无论是否检测到事件，`ai_frame_release()` 都必须释放 mmap 和 dump frame。

退出清理链路：

`sig_handler() 或 AUTO_EXIT_SEC 超时 -> g_running = 0 -> stream_thread() 退出 -> rt_sem_release(g_stream_exit_sem) -> main() 等待完成 -> pipeline_cleanup()`

`pipeline_cleanup() -> ai_motion_thread_stop() -> stream_export_deinit() -> osd_deinit() -> kd_mpi_vicap_stop_stream() -> kd_mpi_sys_unbind() -> kd_mpi_vicap_deinit() -> kd_mpi_venc_stop_chn() -> kd_mpi_venc_destroy_chn() -> kd_mpi_venc_close_fd() -> kd_mpi_vb_exit()`

关键数据传递：
- `g_running` 是主线程、码流线程、AI 线程的共同退出标志。
- `g_stream_exit_sem` 用于通知主线程：VENC 码流线程已经自然退出。
- `g_ai_motion_exit_sem` 用于通知清理函数：AI 运动线程已经自然退出。
- `g_status` 记录初始化进度，清理时只释放已经成功创建的资源。

## 2. 一次完整的数据旅行

典型输入：GC2093 摄像头产生一帧 `1920x1080` 图像；VICAP 同时输出主通道 `1920x1080 NV12` 和 AI 通道 `640x480` 低清帧。

1. 程序从 `main()` 进入。
   `main()` 先注册信号处理函数，然后调用 `vb_init()`。`vb_init()` 根据固定尺寸和块数创建 VB 内存池，为后续 VICAP、VENC、AI dump 提供可复用 buffer。

2. `main()` 调用 `venc_init(VENC_CHN, ENC_BITRATE)`。
   这里创建 H.265 Main Profile、CBR 模式的编码通道。输出不是文件，而是 VENC 内部可被 `kd_mpi_venc_get_stream()` 取出的压缩码流。

3. `main()` 调用 `vicap_config(SENSOR_TYPE)`。
   `vicap_config()` 先尝试首选 GC2093 CSI 配置，如果失败再尝试 fallback。成功后，`vicap_try_config()` 已经完成两路通道设置：主通道用于编码，AI 通道用于低清灰度分析。

4. `main()` 调用 `vi_bind_venc()`。
   主通道 `VICAP_CHN` 被硬件绑定到 `VENC_CHN`。从这一刻起，主通道的图像帧不需要 CPU 拷贝，会由 MPP 直接送入 VENC 编码。

5. `main()` 调用 `vicap_start()`。
   VICAP 开始采集 sensor 图像。主通道帧进入 VENC；AI 通道帧留给 `ai_frame_try_get()` dump。

6. `main()` 调用 `ai_motion_thread_start()`。
   该函数初始化 AI 帧读取和运动适配层，然后创建 RT-Thread 线程 `ai_motion_thread`。线程启动后不断从 `AI_VICAP_CHN` 取低清帧。

7. 一帧 AI 低清图像进入 `ai_frame_try_get()`。
   函数调用 `kd_mpi_vicap_dump_frame()` 得到帧信息，再 mmap Y 平面，填出 `ai_gray_frame_view`。这个视图传给 `motion_adapter_process()`。

8. `motion_adapter_process()` 调用 `motion_detect_process()`。
   当前默认实现是弱符号 stub，结果通常是 `is_motion = 0`，不会产生事件。若后续接入 B 题运动检测算法并覆盖该函数，当检测到运动时会生成 `motion_event_msg`，然后 `ai_motion_thread()` 调用 `osd_set_motion_visible(1, 1000)`。当前 OSD 也是 stub，只打印日志。

9. 同一时刻，主通道帧在硬件链路中进入 VENC。
   `stream_thread()` 周期性调用 `kd_mpi_venc_query_status()` 查询可用 pack 数，再调用 `kd_mpi_venc_get_stream()` 取出编码结果。

10. VENC 输出的 `k_venc_stream` 被转换为 `mpp_stream_frame_desc`。
    `stream_thread()` 逐个读取 pack 的物理地址、长度和类型，填入项目内部描述结构。header pack 和普通 NALU pack 都会被保留。

11. `stream_export_submit()` 接收 `mpp_stream_frame_desc`。
    在 `STREAM_EXPORT_LOCAL_LOG` 模式下，它不写文件、不发网络，只打印日志。普通码流 pack 输出类似 `Get NALU, Size: xxxx bytes [frame #N]`；header pack 输出 SPS/PPS header 大小。

12. `stream_thread()` 调用 `kd_mpi_venc_release_stream()`。
    这一帧的编码码流资源被归还给 VENC。随后线程继续处理下一帧，直到 `g_running` 变为 `0`。

13. 到达 `AUTO_EXIT_SEC` 或收到退出信号后，`main()` 设置 `g_running = 0`。
    码流线程退出并释放 `g_stream_exit_sem`；清理阶段调用 `pipeline_cleanup()`，按逆序停止 AI 线程、VICAP、绑定关系、VENC 和 VB。

最终对外输出：
- 终端日志：包括初始化结果、`VI->VENC bind OK`、`Get NALU, Size: ...`、统计帧率、AI/OSD stub 日志和清理日志。
- 程序退出状态：成功时打印 `Pipeline test PASSED.` 并返回 `0`；失败时打印 `Pipeline test FAILED ...` 并返回错误码。
- 当前版本不生成视频文件、不启动网络服务；码流导出只做本地日志打印。
