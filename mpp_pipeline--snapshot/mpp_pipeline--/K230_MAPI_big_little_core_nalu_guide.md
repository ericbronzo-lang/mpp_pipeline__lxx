# K230 MAPI 大小核通信与 NALU 获取示例

本文以官方 `demo` 为参考，说明 K230 项目中“小核应用如何拿到大核/媒体服务产生的 H.265 NALU 数据”，以及如何把这条链路接到你当前的 RTP/RTSP 发送代码。

## 1. 先明确：官方 demo 没有手写裸 IPC

官方 demo 中没有直接出现 `mailbox`、`rpmsg`、共享内存队列等底层通信代码。大小核之间的通信被 K230 的 MAPI 框架封装了。

应用侧看到的是这一层：

```text
小核用户态应用
  -> kd_mapi_sys_init()
  -> kd_mapi_media_init()
  -> kd_mapi_venc_init()
  -> kd_mapi_venc_registercallback()
  -> kd_mapi_venc_start()
```

底层真实发生的是这一层：

```text
小核应用发 MAPI 控制请求
  -> MAPI/媒体服务处理请求
  -> 大核/媒体子系统驱动 VI/VENC 硬件
  -> 编码后的码流进入 VB/stream buffer
  -> MAPI 把可访问的虚拟地址、长度、PTS 回调给小核应用
```

所以你在应用层不需要自己写“大核发送 NALU、小核接收 NALU”的裸通信逻辑。你需要做的是：注册 VENC 回调，并在回调里取出 `vir_addr / len / pts`。

## 2. 官方 demo 的真实数据流

官方 demo 的核心链路是：

```text
Camera Sensor
  -> VICAP / VI
  -> VENC 硬件编码
  -> kd_mapi_venc_registercallback() 注册的回调
  -> sessionVideoCallback()
  -> H265LiveFrameSource::pushData(data, len, pts)
  -> LiveFrameSource 内部队列
  -> H265LiveFrameSource::parseFrame()
  -> Live555 RTSP/RTP
  -> VLC / ffplay
```

关键代码在官方 demo 的 `src/streaming_player.cpp`：

```cpp
static k_s32 sessionVideoCallback(k_u32 chn_num,
                                  kd_venc_data_s* p_vstream_data,
                                  k_u8 *p_private_data)
{
    int cut = p_vstream_data->status.cur_packs;

    for (int i = 0; i < cut; i++) {
        k_char *pdata = p_vstream_data->astPack[i].vir_addr;
        size_t len = p_vstream_data->astPack[i].len;
        uint64_t pts = p_vstream_data->astPack[i].pts;

        H265LiveFrameSource *h265LiveSource =
            (H265LiveFrameSource*)session_info[chn_num].sessionVideoLiveSource;

        h265LiveSource->pushData((const uint8_t*)pdata, len, pts);
    }

    return 0;
}
```

其中：

- `status.cur_packs`：本次回调包含多少个编码包。
- `astPack[i].vir_addr`：当前编码包在小核可访问地址空间中的虚拟地址。
- `astPack[i].len`：当前编码包长度。
- `astPack[i].pts`：当前编码包时间戳。

如果编码类型是 H.265，那么 `pdata + len` 通常就是可以继续解析/发送的 H.265 码流数据。它可能是一段 Annex-B 数据，也可能包含一个或多个 NALU，具体要看 VENC 输出格式和 SDK 配置。

## 3. 大核侧框架内部伪代码

官方 demo 没有给出真实大核端源码，所以这里用框架内部伪代码帮助理解。

注意：这不是你要编译的代码，只是解释 MAPI 背后的信息流。

```c
/*
 * 大核/媒体服务侧伪代码
 * 实际逻辑由 K230 MAPI 和媒体服务封装。
 */

typedef struct {
    int cmd;
    int venc_chn;
    void *args;
} mapi_cmd_t;

typedef struct {
    void *phys_addr;
    void *mapped_virt_addr_for_client;
    unsigned int len;
    unsigned long long pts;
} encoded_pack_t;

static venc_callback_t g_venc_callback[MAX_VENC_CHN];

void media_service_loop(void)
{
    while (1) {
        mapi_cmd_t cmd = wait_mapi_command_from_small_core();

        switch (cmd.cmd) {
        case MAPI_CMD_VENC_INIT:
            venc_hw_init(cmd.venc_chn, cmd.args);
            break;

        case MAPI_CMD_VENC_REGISTER_CALLBACK:
            g_venc_callback[cmd.venc_chn] = cmd.args;
            break;

        case MAPI_CMD_VENC_BIND_VI:
            bind_vi_to_venc(cmd.venc_chn, cmd.args);
            break;

        case MAPI_CMD_VENC_START:
            venc_hw_start(cmd.venc_chn);
            break;

        default:
            break;
        }
    }
}

void venc_irq_or_worker_on_stream_ready(int venc_chn)
{
    kd_venc_data_s stream;

    memset(&stream, 0, sizeof(stream));

    /*
     * 1. 从 VENC/VB stream buffer 中取出编码结果。
     * 2. 把物理内存映射成小核应用可访问的虚拟地址。
     * 3. 填充 kd_venc_data_s。
     */
    stream.status.cur_packs = get_ready_pack_count(venc_chn);

    for (int i = 0; i < stream.status.cur_packs; i++) {
        encoded_pack_t pack = get_encoded_pack(venc_chn, i);

        stream.astPack[i].vir_addr = pack.mapped_virt_addr_for_client;
        stream.astPack[i].len = pack.len;
        stream.astPack[i].pts = pack.pts;
    }

    /*
     * 通知小核应用：编码数据已经准备好。
     * 应用侧看到的就是 kd_mapi_venc_registercallback() 注册的回调被调用。
     */
    if (g_venc_callback[venc_chn]) {
        notify_small_core_callback(venc_chn, &stream);
    }
}
```

你要记住的重点只有一个：大核/媒体服务侧负责把编码结果准备好，小核应用侧通过 MAPI 回调拿到描述信息。

## 4. 小核侧 MAPI 实战示例

下面是贴近官方 demo 的简化版小核代码。它展示了如何初始化媒体模块、创建 VENC、注册回调，并在回调中拿到 H.265 包。

```cpp
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "mapi_sys_api.h"
#include "mapi_venc_api.h"
#include "mapi_vicap_api.h"
#include "k_vicap_comm.h"

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint64_t pts;
    uint32_t frame_id;
    uint8_t is_keyframe;
} video_packet_t;

static uint32_t g_frame_id = 0;

static int h265_nalu_type(const uint8_t *data, uint32_t len)
{
    const uint8_t *p = data;

    if (data == NULL || len < 2) {
        return -1;
    }

    /*
     * 如果前面带 Annex-B 起始码，需要跳过：
     * 00 00 01 或 00 00 00 01
     */
    if (len >= 5 && data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x00 && data[3] == 0x01) {
        p = data + 4;
    } else if (len >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
               data[2] == 0x01) {
        p = data + 3;
    }

    return (p[0] >> 1) & 0x3f;
}

static int is_h265_keyframe_type(int nal_type)
{
    /*
     * H.265 常见关键 NAL 类型：
     * 19: IDR_W_RADL
     * 20: IDR_N_LP
     * 32: VPS
     * 33: SPS
     * 34: PPS
     */
    return nal_type == 19 || nal_type == 20 ||
           nal_type == 32 || nal_type == 33 || nal_type == 34;
}

static int video_packet_queue_push(const video_packet_t *pkt)
{
    /*
     * 第一版可以在这里把 data 拷贝进你自己的环形队列。
     * 后续优化时，再改成引用 VB buffer 或者使用固定内存池。
     *
     * 这里先留接口，便于和你的 RTP 发送线程对接。
     */
    printf("[queue] frame=%u len=%u pts=%llu key=%u\n",
           pkt->frame_id,
           pkt->len,
           (unsigned long long)pkt->pts,
           pkt->is_keyframe);

    return 0;
}

static k_s32 venc_data_callback(k_u32 chn_num,
                                kd_venc_data_s *p_vstream_data,
                                k_u8 *p_private_data)
{
    if (p_vstream_data == NULL) {
        return -1;
    }

    int pack_count = p_vstream_data->status.cur_packs;

    for (int i = 0; i < pack_count; i++) {
        const uint8_t *data =
            (const uint8_t *)p_vstream_data->astPack[i].vir_addr;
        uint32_t len = (uint32_t)p_vstream_data->astPack[i].len;
        uint64_t pts = p_vstream_data->astPack[i].pts;

        if (data == NULL || len == 0) {
            printf("[venc] empty pack: chn=%u index=%d\n", chn_num, i);
            continue;
        }

        int nal_type = h265_nalu_type(data, len);

        video_packet_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.data = data;
        pkt.len = len;
        pkt.pts = pts;
        pkt.frame_id = g_frame_id++;
        pkt.is_keyframe = is_h265_keyframe_type(nal_type);

        printf("[venc] chn=%u pack=%d/%d len=%u pts=%llu nal_type=%d\n",
               chn_num,
               i + 1,
               pack_count,
               len,
               (unsigned long long)pts,
               nal_type);

        video_packet_queue_push(&pkt);
    }

    return 0;
}

int setup_venc_callback_example(int venc_chn)
{
    kd_venc_callback_s cb;
    memset(&cb, 0, sizeof(cb));

    cb.p_private_data = NULL;
    cb.pfn_data_cb = venc_data_callback;

    /*
     * 这一步是小核获取大核/媒体服务编码数据的关键：
     * 你注册了回调，后续 VENC 有码流时，MAPI 会调用 venc_data_callback()。
     */
    int ret = kd_mapi_venc_registercallback(venc_chn, &cb);
    if (ret != K_SUCCESS) {
        printf("kd_mapi_venc_registercallback failed, ret=0x%x\n", ret);
        return -1;
    }

    return 0;
}
```

这个示例里，小核拿到 NALU 的关键不是 `recv()`，也不是自己读共享内存，而是：

```cpp
kd_mapi_venc_registercallback(venc_chn, &cb);
```

然后在回调里读取：

```cpp
p_vstream_data->astPack[i].vir_addr
p_vstream_data->astPack[i].len
p_vstream_data->astPack[i].pts
```

## 5. 和你当前 RTSP/RTP 代码的对接方式

你现在的文件版 RTSP/RTP 逻辑大概是：

```text
read_whole_file()
  -> load_annexb_nalus()
  -> g_nalus[]
  -> rtp_sender_thread()
  -> send_h265_nalu_rtp()
```

接入真实 VENC 之后，应该改成：

```text
kd_mapi_venc_registercallback()
  -> venc_data_callback()
  -> video_packet_queue_push()
  -> rtp_sender_thread()
  -> video_packet_queue_pop()
  -> send_h265_nalu_rtp()
```

第一版推荐这样做：

```text
VENC 回调线程：
  1. 取 vir_addr / len / pts
  2. memcpy 到你自己的固定大小队列 buffer
  3. 快速返回，不能在回调里直接 sendto 太久

RTP 发送线程：
  1. 从队列取 video_packet_t
  2. 判断是否 H.265 Annex-B 起始码
  3. 拆 NALU 或直接发送单个 NALU
  4. 调用 send_h265_nalu_rtp()
```

后续优化时再减少拷贝：

```text
VENC buffer / VB buffer
  -> 只传 buffer 描述符、物理地址、长度、PTS
  -> RTP 线程发送完成后释放或归还 buffer
```

但这个优化要非常谨慎，因为 RT-Smart 进程是隔离地址空间，不能随便把一个进程里的普通虚拟地址传给另一个进程。跨进程时应该传“共享内存句柄/物理地址/offset/长度”，而不是直接传 `void *`。

## 6. 一个最小队列接口模板

下面这个接口可以作为你从“文件 NALU”切到“实时 VENC NALU”的中间层。

```c
typedef struct {
    uint8_t *data;
    uint32_t len;
    uint64_t pts;
    uint32_t frame_id;
    uint8_t is_keyframe;
} video_packet_copy_t;

int video_queue_init(void);

/*
 * 在 VENC 回调里调用。
 * 必须快：队列满时建议丢包，不能长时间阻塞编码回调。
 */
int video_queue_push_copy(const uint8_t *data,
                          uint32_t len,
                          uint64_t pts,
                          uint32_t frame_id,
                          uint8_t is_keyframe);

/*
 * 在 RTP 发送线程里调用。
 * 可以阻塞等待，但不要影响 RTSP 控制线程。
 */
int video_queue_pop(video_packet_copy_t *out_pkt, int timeout_ms);

void video_queue_release(video_packet_copy_t *pkt);
```

你的 `rtp_sender_thread()` 从：

```c
const nalu_t *nalu = &g_nalus[idx];
send_h265_nalu_rtp(sock, &dest, nalu->data, nalu->len, ...);
```

改成：

```c
video_packet_copy_t pkt;

if (video_queue_pop(&pkt, 1000) == 0) {
    send_h265_nalu_rtp(sock,
                       &dest,
                       pkt.data,
                       pkt.len,
                       &seq,
                       rtp_timestamp_from_pts(pkt.pts),
                       ssrc,
                       1);

    video_queue_release(&pkt);
}
```

## 7. 调试方法

先不要急着接完整 RTSP。先在回调里打印：

```cpp
printf("[venc] chn=%u packs=%d len=%u pts=%llu nal_type=%d\n",
       chn_num,
       p_vstream_data->status.cur_packs,
       len,
       (unsigned long long)pts,
       nal_type);
```

H.265 常见类型：

```text
32 = VPS
33 = SPS
34 = PPS
19 = IDR_W_RADL
20 = IDR_N_LP
1  = 普通非关键帧 slice
```

如果你能持续看到：

```text
len > 0
pts 递增
偶尔出现 VPS/SPS/PPS/IDR
```

说明小核已经成功拿到大核/媒体服务输出的 H.265 数据。

## 8. 验收标准

完成这一阶段后，你应该能说清楚：

- 官方 demo 的大小核通信由 `kd_mapi_*` 封装，不需要应用层直接写 mailbox/rpmsg。
- 小核获取 H.265 数据的入口是 `kd_mapi_venc_registercallback()`。
- 真正的数据在 `kd_venc_data_s.astPack[i]` 中。
- `vir_addr` 是数据地址，`len` 是数据长度，`pts` 是时间戳。
- 你当前文件版 RTSP/RTP 代码要替换的是“文件读取和 NALU 数组”，不是 RTP/H.265 分片函数本身。
- VENC 回调里不要做耗时操作，应该快速投递到队列，由 RTP 线程发送。

