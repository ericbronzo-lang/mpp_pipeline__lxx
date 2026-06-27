# AI 低清旁路修改指南

面向对象：有完整 `k230_sdk`、能编译并上板验证的代码区 agent。

目标：在不回退、不重写现有 1080P H.265 主链路的前提下，新增给队员 B 使用的 640x480 灰度运动检测旁路，并验证 K230 VICAP/VI 是否可以直接输出 640x480 灰度图。

当前 `mpp_pipeline` 文件按任务目标命名：`media_*` 是大核 MPP 底层媒体能力，`stream_*` 是码流/小核推流对接面，`motion_*` 是 AI 运动检测对接面。

## 1. 当前代码边界

当前代码位于：

```text
OS 与多媒体核心管线/mpp_pipeline
```

现有文件职责：

| 文件 | 当前职责 | 本次处理原则 |
| --- | --- | --- |
| `mpp_pipeline.c` | 主流程编排：VB、VENC、OSD、VICAP、bind、start、stream thread、cleanup | 只接入 AI 线程启动，不塞算法细节 |
| `mpp_pipeline.h` | 全局宏、通道号、buffer 尺寸、状态机、函数声明 | 新增 AI 通道常量、类型声明、模块函数声明 |
| `mpp_types.h` | 跨模块数据结构 | 新增 B 的灰度帧视图和运动检测结果类型 |
| `media_vb.c` | 初始化 VB pool | 为 AI 低清旁路增加 buffer 规划 |
| `media_vicap.c` | 配置 VICAP device 和当前 1080P 主通道 | 拆出通道配置 helper，配置主通道 + AI 通道 |
| `media_bind.c` | 绑定 VI 主通道到 VENC | 保持只绑定主通道，不绑定 AI 通道 |
| `media_venc.c` | H.265 VENC 初始化和码流获取 | 不加入 AI 算法，不改网络 |
| `stream_export.c` | 当前仅打印 NALU，后续给小核 RTSP 替换内部实现 | 本次不改 |
| `media_osd.c` | OSD stub，提供 `osd_set_motion_visible()` | 本次由 AI 线程调用该函数即可 |
| `media_cleanup.c` | 逆序释放资源 | 新增 AI 线程后，先停 AI 线程再停 VICAP |
| `SConscript` | 显式源码白名单 | 新增 `.c` 文件必须手动加入 |

禁止事项：

- 不把 AI 算法写进 `media_vicap.c` 或 `media_venc.c`。
- 不修改 `vi_bind_venc()` 去绑定 AI 通道。
- 不在大核实现 RTSP/RTP/网络发送。
- 不 CPU 遍历 1080P 原始帧画 OSD。
- 不把 `SConscript` 改成 `Glob('*.c')`。

## 2. 先查 SDK：能否直接输出灰度

在 `k230_sdk/src/big/mpp` 中先搜索这些内容：

```bash
grep -R "PIXEL_FORMAT_.*400\|PIXEL_FORMAT_.*GRAY\|PIXEL_FORMAT_.*GREY\|PIXEL_FORMAT_.*MONO\|PIXEL_FORMAT_.*Y" -n include userapps | head -50
grep -R "kd_mpi_vicap_dump_frame\|kd_mpi_vicap_dump_release\|VICAP_DUMP" -n . | head -80
grep -R "VICAP_CHN_ID_2\|set_chn_attr" -n userapps sample* src 2>/dev/null | head -80
```

决策：

- 如果 SDK 明确支持 Y-only / grayscale 像素格式，并且 VICAP channel 可以设置该格式，则 AI 通道优先直接输出 640x480 灰度。
- 如果 SDK 不支持，或板端 `kd_mpi_vicap_init()` / `start_stream()` 失败，则回退到 640x480 NV12，并只把 Y 平面传给 B。
- 回退 NV12-Y 仍满足 B 的运动检测输入需求：B 只处理亮度平面，不处理 UV。

建议宏：

```c
#define AI_WIDTH  640
#define AI_HEIGHT 480
#define AI_BUF_CNT 6
#define AI_VICAP_CHN VICAP_CHN_ID_2

#ifndef AI_USE_Y_ONLY_FORMAT
#define AI_USE_Y_ONLY_FORMAT 0
#endif

#if AI_USE_Y_ONLY_FORMAT
#define AI_PIXEL_FORMAT     /* 填 SDK 真实灰度格式 */
#define AI_CHN_BUF_SIZE     ALIGN_UP(AI_WIDTH * AI_HEIGHT, 0xFFF)
#else
#define AI_PIXEL_FORMAT     PIXEL_FORMAT_YUV_SEMIPLANAR_420
#define AI_CHN_BUF_SIZE     ALIGN_UP(AI_WIDTH * AI_HEIGHT * 3 / 2, 0xFFF)
#endif
```

注意：不要在未确认 SDK 符号时硬写 `PIXEL_FORMAT_YUV_400`。先查头文件。

## 3. 新增公共接口

在 `mpp_types.h` 新增：

```c
#define AI_GRAY_MAX_WIDTH    640
#define AI_GRAY_MAX_HEIGHT   480

typedef struct {
    k_u64 frame_id;
    k_u64 timestamp_ms;
    k_u32 width;
    k_u32 height;
    k_u32 stride;
    const k_u8 *y;
} ai_gray_frame_view;

typedef struct {
    k_u32 is_motion;
    k_u32 motion_score;
} motion_detect_result;
```

B 的算法入口固定在 `motion_detect.h`：

```c
k_s32 motion_detect_process(const ai_gray_frame_view *frame,
                            motion_detect_result *result);
```

约定：

- `frame->y` 是当前进程内可读的 AI 低清通道 NV12 Y 平面地址，可直接当灰度图输入，不跨核传递。
- B 按 `frame->stride` 访问每一行有效的 `frame->width` 字节，不能假设 `stride == width`。
- B 不保存 `frame->y`，只在函数调用期间使用；需要跨帧比较时复制 Y 数据到自己的缓存。
- B 需要前后帧比较时，在自己的模块里保存上一帧副本。
- B 返回 `is_motion` 和 `motion_score`，不直接操作 OSD、VICAP、VENC、RT-Thread 线程或帧释放。

## 4. 推荐新增模块

### `motion_ai_frame.c`

职责：只负责 AI 通道取帧、映射 Y 平面、释放帧。

建议接口：

```c
k_s32 ai_frame_channel_init(void);
k_s32 ai_frame_try_get(ai_gray_frame_view *view, void **handle);
k_s32 ai_frame_release(void *handle);
void ai_frame_channel_deinit(void);
```

实现要求：

- 使用 SDK 官方 sample 中确认过的取帧 API。
- 如果路线是 `kd_mpi_vicap_dump_frame()`，必须配套调用 release API。
- 如果需要 mmap，处理完必须 munmap。
- `ai_frame_try_get()` 拿不到帧时返回错误码，不阻塞太久。
- 上层处理完每一帧后必须调用 `ai_frame_release(handle)`。

### `motion_adapter.c`

职责：只负责调用 B 的 `motion_detect_process()`，把结果转成 `motion_event_msg`。

建议接口：

```c
k_s32 motion_adapter_init(void);
k_s32 motion_adapter_process(const ai_gray_frame_view *frame,
                             motion_event_msg *event,
                             k_bool *has_event);
void motion_adapter_deinit(void);
```

实现要求：

- 在 B 代码未合入前，由 `motion_detect_default.c` 提供弱符号帧差示例版 `motion_detect_process()`，用于验证事件链路。
- B 合入时提供同名强符号实现即可覆盖默认弱符号；不需要改 AI 线程或适配层。
- 检测到运动后生成递增 `event_id`。
- 默认 `event.osd_duration_ms = 1000`。

### `motion_thread.c`

职责：独立 AI 线程，串起取帧、B 算法、OSD 触发。

建议接口：

```c
k_s32 ai_motion_thread_start(void);
void ai_motion_thread_stop(void);
```

线程流程：

```text
while running:
    ai_frame_try_get()
    motion_adapter_process()
    if has_event:
        osd_set_motion_visible(1, event.osd_duration_ms)
    ai_frame_release()
```

要求：

- AI 线程不能阻塞 1080P 主链路。
- 算法跟不上时，优先丢帧/跳帧，而不是压住 VICAP 或 VENC。
- `media_cleanup.c` 里必须先停 AI 线程，再 stop/deinit VICAP。

## 5. 修改现有模块

### `mpp_pipeline.h`

新增：

- AI 通道常量：`AI_WIDTH`、`AI_HEIGHT`、`AI_BUF_CNT`、`AI_VICAP_CHN`。
- AI buffer size 宏。
- 新增模块函数声明。
- B 算法入口声明。

### `media_vb.c`

新增 AI 低清 buffer pool，推荐：

```c
config.max_pool_cnt = 3;
config.comm_pool[2].blk_cnt  = AI_BUF_CNT;
config.comm_pool[2].blk_size = AI_CHN_BUF_SIZE;
config.comm_pool[2].mode     = VB_REMAP_MODE_NOCACHE;
```

如果 SDK sample 显示 VICAP channel 自带 buffer 不需要额外公共 VB pool，也可以不加 pool，但必须在验证记录中说明依据。

### `media_vicap.c`

把当前单通道配置拆为 helper：

```c
static k_s32 vicap_set_channel_attr(...);
```

配置两个通道：

- 主通道：`VICAP_CHN_ID_0`，1920x1080，NV12，不缩放，后续 bind 到 VENC。
- AI 通道：`VICAP_CHN_ID_2`，640x480，灰度优先，NV12-Y 兜底，不 bind VENC。

AI 通道缩放：

- 如果 SDK channel 支持 scale，从 sensor 输出缩放到 640x480。
- 如果不支持 1920x1080 到 640x480，先记录失败原因，再尝试 SDK sample 推荐的低清通道配置。

### `mpp_pipeline.c`

建议顺序：

```text
vb_init()
venc_init()
osd_init()
vicap_config()
vi_bind_venc()
vicap_start()
ai_motion_thread_start()
stream_export_init()
pthread_create(stream_thread)
```

AI 线程必须在 `vicap_start()` 后启动。

### `media_cleanup.c`

在 `pipeline_cleanup()` 开头加入：

```c
ai_motion_thread_stop();
```

位置必须早于：

```c
kd_mpi_vicap_stop_stream()
kd_mpi_vicap_deinit()
```

### `SConscript`

显式加入：

```python
'motion_ai_frame.c',
'motion_detect_default.c',
'motion_adapter.c',
'motion_thread.c',
```

不要改成 `Glob('*.c')`。

## 6. 板端验证清单

### 构建验证

```bash
cd /home/ubuntu/workspace/big_core
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" scons -j4
```

必须确认：

- 新增 `.c` 文件参与编译。
- `motion_detect_process()` 未合入 B 代码时也能链接，并使用默认帧差示例实现。
- SDK 取帧和释放 API 签名正确。

### 主链路回归

运行：

```bash
./big_app.elf
```

必须确认：

- 1080P H.265 NALU 仍持续输出。
- `vi_bind_venc()` 仍只绑定主通道到 VENC。
- AI 线程启动后主码流 fps 无明显下降。
- Ctrl+C 后线程可退出，cleanup 完整。

### AI 通道验证

必须打印并记录：

- AI 通道 ID。
- AI 输出宽高。
- AI pixel format。
- AI stride。
- 是否真灰度。
- 若非真灰度，是否回退 NV12-Y。

验收标准：

- B 能收到 `width=640`、`height=480` 的灰度输入。
- `frame->y` 非空。
- B 的算法处理完后，队长侧能调用 `osd_set_motion_visible(1, 1000)`。
- 每一帧都能 release，不出现 buffer 越占越多。

## 7. 风险点

- K230 SDK 的 VICAP dump API 字段名必须以本地头文件/sample 为准，不能凭空猜。
- 真灰度 pixel format 不一定存在；不存在就用 NV12-Y。
- AI 线程取帧超时是正常情况，不要把超时当成致命错误。
- B 的前后帧比较必须复制上一帧 Y 数据，不能保存硬件 buffer 指针。
- OSD 叠加到检测后的后续帧是允许的；Week2 先打通闭环，后续再优化延迟。
