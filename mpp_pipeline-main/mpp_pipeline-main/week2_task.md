# Week 2 队长任务书：大核媒体管线与大小核对接准备

更新时间：2026-06-08

> 给代码编辑工作空间里的 agent 使用：本任务书只要求改大核 RT-Smart 侧 `mpp_pipeline` 代码和配套接口文档。不要重写 Week 1 主链路，不要从零实现 RTSP，不要把网络服务放到大核。小核 Linux RTSP demo 由队员 A 负责；队长只需要提供大核侧稳定码流、接口描述和对接点。

## 0. 背景与目标

当前 Week 1 代码位于：

```text
OS 与多媒体核心管线/mpp_pipeline
```

已经跑通的大核主链路：

```text
GC2093 Sensor -> VICAP/VI -> kd_mpi_sys_bind -> VENC(H.265) -> stream_thread 获取 NALU
```

Week 2 队长目标：

1. 稳定大核 H.265 码流线程，消除每帧 `malloc/free` 和永久阻塞退出风险。
2. 抽象出大核码流出口，方便后续小核 Linux RTSP 服务通过 MAPI/IPC/共享内存消费。
3. 形成 `接口说明.md`，让队员 A/B 不需要读 `venc_module.c` 内部也能对接。
4. 验证或至少写清低分辨率 AI 旁路通道方案。
5. 预留硬件 OSD Region 控制接口，禁止 CPU 逐像素画 1080P。

最终 Week 2 队长验收结果不是“完整产品”，而是：

```text
大核主链路稳定 + 小核对接字段清楚 + AI/OSD/快照接口清楚 + 板端日志能证明没有把 Week 1 跑通的链路弄坏
```

## 1. 必须遵守的架构边界

大核 RT-Smart 负责：

- VB / VICAP / VI。
- VENC(H.265)。
- OSD Region。
- 低分辨率 AI 旁路帧。
- 移动侦测事件低延迟闭环。

小核 Linux 负责：

- RTSP server。
- RTP/H.265 打包与网络发送。
- Wi-Fi / Ethernet。
- VLC / ffplay 拉流入口。
- 部署、日志、`sharefs`、文件辅助。

大小核之间：

- 优先使用官方 MAPI/IPC + 共享内存。
- 只传控制消息、共享内存句柄、物理地址或官方支持的数据结构。
- 不传普通虚拟指针。

禁止事项：

- 不从零实现 RT-Smart 侧 RTSP。
- 不在大核上写 UDP/RTP 网络发送作为主路线。
- 不 `memcpy` 1080P 原始帧给小核。
- 不在 1080P 像素数组里 CPU 循环画 OSD。
- 不随意修改 VB/VICAP/VENC 参数；必须改时写明原因并重新做 10 分钟稳定性测试。
- 不改 `SConscript` 为 `Glob('*.c')`；新增 `.c` 文件必须显式加入白名单。

## 2. 当前文件职责

```text
mpp_pipeline.c      程序入口、信号处理、初始化编排、600 秒自动退出、PASS/FAIL 日志
mpp_pipeline.h      公共配置、MPP SDK 头、通道 ID、buffer size、状态机、函数声明
vb_module.c         VB pool 配置和初始化
venc_module.c       VENC 创建/启动，stream_thread 获取 H.265 NALU
vicap_module.c      GC2093 查询、VICAP 配置、CSI fallback、启动
bind_module.c       VI/VICAP 到 VENC 的硬件绑定
cleanup_module.c    按状态机逆序释放 MPP 资源
SConscript          源码白名单与 MPP include/lib 配置
```

## 3. 建议新增文件

代码文件：

```text
mpp_types.h         Week 2 跨模块公共数据结构，只放轻量 struct 和常量
stream_export.c     大核码流出口适配层，先做本地日志回调，不做小核网络
osd_module.c        OSD Region 控制接口 stub 或最小验证实现
ai_frame_module.c   低分辨率 AI 旁路方案验证代码，若 API 不确定可先只写方案文档
```

文档文件：

```text
接口说明.md          给队员 A/B 对接使用，写清码流、事件、快照、OSD、低分辨率帧字段
板端验证记录.md      记录每次上板命令、日志片段、结果、遗留问题
```

如果 SDK API 不明确，不要硬编不可用函数。可以先创建文档和接口草案，并在代码里只实现已经确认可编译的部分。

新增 `.c` 后必须修改：

```text
OS 与多媒体核心管线/mpp_pipeline/SConscript
```

只在 `src = [...]` 中显式加入文件名。

## 4. Task 1：稳定化 `stream_thread`

目标：让 VENC 码流线程能长期运行、能可靠退出、不会每帧动态分配。

修改文件：

```text
OS 与多媒体核心管线/mpp_pipeline/venc_module.c
OS 与多媒体核心管线/mpp_pipeline/mpp_pipeline.h
```

实现要求：

1. 增加常量：

```c
#define VENC_MAX_PACKS        8
#define VENC_GET_STREAM_TIMEOUT_MS  200
```

2. `stream_thread()` 内不要每帧 `malloc/free`：

```c
k_venc_pack packs[VENC_MAX_PACKS];
k_venc_stream output;
memset(&output, 0, sizeof(output));
output.pack = packs;
```

3. 每轮查询 `cur_packs` 后限制 pack 数：

```c
output.pack_cnt = status.cur_packs;
if (output.pack_cnt == 0)
    output.pack_cnt = 1;
if (output.pack_cnt > VENC_MAX_PACKS)
    output.pack_cnt = VENC_MAX_PACKS;
```

4. `kd_mpi_venc_get_stream()` 使用有限超时：

```c
ret = kd_mpi_venc_get_stream(chn, &output, VENC_GET_STREAM_TIMEOUT_MS);
```

5. 超时或暂时无流时不要直接退出线程；如果 `g_running == 0` 则退出，否则继续循环。具体错误码以 SDK 实测为准，日志不要刷屏。

6. 保留 Week 1 关键日志格式：

```text
[MPP] Get NALU, Size: xxxx bytes  [frame #N]
[MPP] === Stats: ...
[MPP] Stream thread exit. Total: ...
```

7. 新增一条启动日志，便于确认 Week 2 版本：

```text
[MPP] Stream export mode: local-log, timeout=200ms, max_packs=8
```

验收标准：

- Ctrl+C 后 `pthread_join()` 能返回。
- 600 秒自动退出后能执行 `Cleanup complete`。
- 10 分钟内无 malloc 失败日志。
- 平均帧率不低于 15fps。

## 5. Task 2：抽象大核码流出口

目标：把“拿到 NALU 后只打印”的逻辑拆出一层，为后续小核 RTSP 对接留接口。

新增文件：

```text
OS 与多媒体核心管线/mpp_pipeline/mpp_types.h
OS 与多媒体核心管线/mpp_pipeline/stream_export.c
```

修改文件：

```text
OS 与多媒体核心管线/mpp_pipeline/mpp_pipeline.h
OS 与多媒体核心管线/mpp_pipeline/venc_module.c
OS 与多媒体核心管线/mpp_pipeline/SConscript
```

`mpp_types.h` 建议内容：

```c
#ifndef MPP_TYPES_H
#define MPP_TYPES_H

#include "k_type.h"
#include "k_venc_comm.h"

#define MPP_MAX_STREAM_PACKS 8

typedef enum {
    STREAM_EXPORT_LOCAL_LOG = 0,
    STREAM_EXPORT_RESERVED_IPC = 1
} stream_export_mode;

typedef struct {
    k_u64 phys_addr;
    k_u64 virt_addr;
    k_u32 len;
    k_u32 type;
} mpp_stream_pack_desc;

typedef struct {
    k_u32 chn;
    k_u64 pts;
    k_u32 pack_cnt;
    mpp_stream_pack_desc packs[MPP_MAX_STREAM_PACKS];
} mpp_stream_frame_desc;

typedef struct {
    k_u32 event_id;
    k_u64 detect_time_ms;
    k_u32 motion_score;
    k_u32 osd_duration_ms;
    k_u32 request_snapshot;
} motion_event_msg;

typedef struct {
    k_u32 event_id;
    k_u64 frame_time_ms;
    k_u32 source_chn;
    char path_hint[64];
} snapshot_request_msg;

#endif
```

`stream_export.c` 建议接口：

```c
#include "mpp_pipeline.h"

k_s32 stream_export_init(stream_export_mode mode)
{
    LOG("Stream export init: mode=%d", mode);
    return 0;
}

k_s32 stream_export_submit(const mpp_stream_frame_desc *frame)
{
    if (!frame)
        return -1;

    for (k_u32 i = 0; i < frame->pack_cnt; i++) {
        if (frame->packs[i].type != K_VENC_HEADER)
            LOG("Get NALU, Size: %u bytes", frame->packs[i].len);
        else
            LOG("Get NALU (SPS/PPS header), Size: %u bytes", frame->packs[i].len);
    }

    return 0;
}

void stream_export_deinit(void)
{
    LOG("Stream export deinit");
}
```

注意：

- `stream_export_submit()` 先只做本地日志，不做小核 IPC。
- `virt_addr` 只能作为大核本进程调试字段，不能跨核使用。
- 后续小核对接时，改的是 `stream_export_submit()` 的内部实现，不要让 VENC 线程直接写网络。

在 `stream_thread()` 中构造 `mpp_stream_frame_desc`：

```c
mpp_stream_frame_desc frame;
memset(&frame, 0, sizeof(frame));
frame.chn = chn;
frame.pts = output.pack[i].pts; /* 如果 SDK 字段名不同，以 k_venc_pack 实际定义为准 */
frame.pack_cnt = output.pack_cnt;
```

如果 `k_venc_pack` 没有 `pts`、`phys_addr` 或 `vir_addr` 字段，不能猜字段名；先只填 `len` 和 `type`，并在 `接口说明.md` 里记录“待按 SDK 结构体确认”。

验收标准：

- 编译通过。
- 日志仍能看到 NALU 大小。
- `stream_thread()` 中网络/IPC 细节为 0，只有调用 `stream_export_submit()`。

## 6. Task 3：写 `接口说明.md`

目标：给队员 A/B 一个稳定对接契约。

新增文件：

```text
OS 与多媒体核心管线/mpp_pipeline/接口说明.md
```

必须包含以下章节：

1. 大小核职责边界。
2. H.265 码流出口字段。
3. 运动事件消息字段。
4. 快照请求字段。
5. OSD 控制函数。
6. 低分辨率 AI 帧字段。
7. 禁止事项。
8. Week 2 对接顺序。

H.265 码流出口字段必须写清：

```text
chn              VENC 通道 ID
pts/time         编码时间戳，字段名以 SDK pack 结构体为准
pack_cnt         pack 数量
pack[].len       每个 pack 长度
pack[].type      K_VENC_HEADER 或普通码流
pack[].phys_addr 后续跨核共享优先字段，如果 SDK 能提供
release          小核消费完成后必须有释放/归还机制，不能永久占用 VENC buffer
```

OSD 控制函数草案：

```c
k_s32 osd_init(void);
k_s32 osd_set_motion_visible(k_u32 visible, k_u32 duration_ms);
void osd_deinit(void);
```

运动事件草案：

```text
event_id
detect_time_ms
motion_score
osd_duration_ms
request_snapshot
```

验收标准：

- 队员 A 能据此知道小核 RTSP 需要向大核要什么字段。
- 队员 B 能据此知道运动侦测触发后调用什么接口或发什么消息。

## 7. Task 4：低分辨率 AI 旁路方案

目标：明确 640x480 或类似低分辨率帧如何从大核拿到，供队员 B 做帧差法。

优先级：先文档，后代码。API 不确认时不要硬写。

新增或修改：

```text
OS 与多媒体核心管线/mpp_pipeline/接口说明.md
OS 与多媒体核心管线/mpp_pipeline/ai_frame_module.c    可选
OS 与多媒体核心管线/mpp_pipeline/mpp_pipeline.h         可选
OS 与多媒体核心管线/mpp_pipeline/SConscript             可选
```

方案文档必须回答：

- 是否使用 VICAP/VI 第二通道。
- 目标分辨率，推荐 640x480。
- 像素格式，推荐 NV12，只处理 Y 分量。
- 帧率，推荐 5 到 15fps，不能反压主链路。
- 获取帧后谁负责释放。
- AI 处理跟不上时丢旧帧还是阻塞，推荐丢旧帧保最新帧。

如果实现代码，建议接口：

```c
k_s32 ai_frame_channel_init(void);
k_s32 ai_frame_try_get(/* output frame desc */);
k_s32 ai_frame_release(/* frame handle */);
void ai_frame_channel_deinit(void);
```

验收标准：

- 最低标准：`接口说明.md` 写清方案和待确认 API。
- 理想标准：板端日志能打印低分辨率帧尺寸、地址、时间戳。

## 8. Task 5：OSD Region 控制接口

目标：为 `Motion Detected!` 提供硬件 OSD 开关接口。

新增或修改：

```text
OS 与多媒体核心管线/mpp_pipeline/osd_module.c
OS 与多媒体核心管线/mpp_pipeline/mpp_pipeline.h
OS 与多媒体核心管线/mpp_pipeline/SConscript
```

建议接口：

```c
k_s32 osd_init(void);
k_s32 osd_set_motion_visible(k_u32 visible, k_u32 duration_ms);
void osd_deinit(void);
```

实现策略：

- 如果已经确认 K230 Region / VENC OSD API，做最小可见矩形或文字位图。
- 如果 API 不确定，先做可编译 stub，日志清楚说明：

```text
[MPP] OSD init stub: waiting for Region API wiring
[MPP] OSD motion visible=1 duration=1000ms
```

stub 不能假装已经完成硬件 OSD；`板端验证记录.md` 必须标明状态是 stub 还是真实硬件 OSD。

验收标准：

- 不改 1080P 像素数组。
- 真实实现时，调用接口后编码画面能显示/隐藏 OSD。
- stub 实现时，编译通过，接口可供队员 B 先联调事件链路。

## 9. Task 6：板端验证记录

新增文件：

```text
OS 与多媒体核心管线/mpp_pipeline/板端验证记录.md
```

每次上板后按这个模板记录：

```markdown
## YYYY-MM-DD HH:mm - 测试名称

构建命令：

```bash
cd /home/ubuntu/workspace/big_core
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" scons -j4
```

运行命令：

```bash
./big_app.elf
```

预期：

- NALU 持续输出。
- 10 分钟不死机。
- Ctrl+C 或自动退出后 cleanup 完整。

关键日志：

```text
[MPP] Stream export mode: local-log, timeout=200ms, max_packs=8
[MPP] Get NALU, Size: ...
[MPP] === Stats: ...
[MPP] Stream thread exit. Total: ...
[MPP] Cleanup complete
[MPP] Pipeline test PASSED.
```

结果：

- PASS 或 FAIL。
- 平均 fps。
- 是否有异常日志。
- 下一步问题。
```

验收标准：

- 至少记录一次短测。
- Week 2 末记录一次 10 分钟稳定性测试。

## 10. 推荐执行顺序

按这个顺序交给 agent 做：

1. 先做 Task 1，稳定 `stream_thread()`。
2. 做 Task 2，增加 `mpp_types.h` 和 `stream_export.c`，把码流出口抽象出来。
3. 做 Task 3，写 `接口说明.md`。
4. 做 Task 5，先提供 OSD 接口 stub；若 API 已确认，再接真实 Region。
5. 做 Task 4，写低分辨率旁路方案；若 API 已确认，再加代码。
6. 做 Task 6，写并维护 `板端验证记录.md`。
7. 每新增 `.c` 文件，同步更新 `SConscript`。
8. 每次上板前先编译，板端运行后记录日志。

## 11. 编译与运行

开发机构建命令：

```bash
cd /home/ubuntu/workspace/big_core
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" scons -j4
```

板端运行：

```bash
./big_app.elf
```

短测通过标准：

```text
[MPP] VB pool init OK
[MPP] VENC chn=0 init OK
[MPP] VICAP dev=0 chn=0 init OK
[MPP] VI->VENC bind OK
[MPP] VICAP stream started
[MPP] Stream export mode: local-log, timeout=200ms, max_packs=8
[MPP] Get NALU, Size: xxxx bytes
```

长测通过标准：

```text
[MPP] Auto-exit after 600 seconds.
[MPP] Stream thread exit. Total: ...
[MPP] Cleanup complete
[MPP] Pipeline test PASSED.
```

## 12. Week 2 队长交付清单

- [ ] `stream_thread()` 无每帧 `malloc/free`。
- [ ] `kd_mpi_venc_get_stream()` 不再永久阻塞。
- [ ] 大核码流出口抽象为 `stream_export_submit()`。
- [ ] 新增 `mpp_types.h`，包含码流、运动事件、快照请求结构。
- [ ] 新增或更新 `接口说明.md`。
- [ ] 明确大核 VENC 到小核 RTSP 的字段需求和 buffer 生命周期问题。
- [ ] 低分辨率 AI 旁路方案写清，最好已能打印帧信息。
- [ ] OSD 控制接口存在；真实硬件实现或 stub 状态写清。
- [ ] `SConscript` 保持显式源码白名单。
- [ ] `板端验证记录.md` 至少有一次短测记录，Week 2 末有一次 10 分钟记录。

## 13. 和队友的对接话术

给队员 A：

```text
我这周会把大核 VENC 码流整理成 frame/pack 描述。你先跑通小核 Linux 官方 RTSP demo，找 H.265 数据入口。我们对齐字段：通道、时间戳、NALU 类型、长度、共享内存/物理地址、释放机制。不要等我写网络，我这边不做 RT-Smart RTSP。
```

给队员 B：

```text
我会提供低分辨率帧方案和 OSD 控制接口。你先把帧差法做成输入 Y 分量、输出 motion_score 的函数。运动事件优先在大核内触发 OSD，不绕小核，以保护 300ms 延迟。
```

给全队：

```text
Week 2 目标不是全功能完成，而是大小核路径定准：小核 RTSP demo 能跑，大核主链路稳定，大核到小核码流字段明确，AI/OSD/快照接口能联调。
```
