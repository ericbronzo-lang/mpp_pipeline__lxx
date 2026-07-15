#ifndef MPP_PIPELINE_H
#define MPP_PIPELINE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#include <rtthread.h>

#include "k_module.h"
#include "k_type.h"
#include "k_vb_comm.h"
#include "k_video_comm.h"
#include "k_sys_comm.h"
#include "mpi_vb_api.h"
#include "mpi_venc_api.h"
#include "mpi_sys_api.h"
#include "mpi_vicap_api.h"
#include "k_venc_comm.h"
#include "mpp_types.h"
#include "motion_detect.h"

/* GC2093 传感器 (开发板标注 TYS-2093-V31)
 * SDK 示例和多数 K230 板卡把 GC2093 接在 CSI2；CSI0 配置会在 kd_mpi_vicap_init()
 * 阶段出现 "iic read chip id err"。默认优先 CSI2，并在运行时回退尝试其它 CSI。
 */
#define SENSOR_TYPE  GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR  /* = 52 */

/* 编码参数 */
#define ENC_WIDTH    1920
#define ENC_HEIGHT   1080
#define ENC_BITRATE  4000        /* kbps */
#define SRC_FPS      30          /* VENC 通道输入帧率参数；当前 GC2093/VICAP 实际输入约 30fps */
#define DST_FPS      15          /* VENC 目标输出帧率参数；低延迟 RTSP 档使用 15fps */
#define VICAP_OUTPUT_FPS 15      /* VICAP 通道输出帧率；0 表示使用 sensor 原始帧率 */
#define VENC_GOP     DST_FPS     /* 约 1 秒一个 I 帧，避免 15fps 下 GOP30 拉长到 2 秒 */

/* VB 池配置 */
#define INPUT_BUF_CNT   6
#define OUTPUT_BUF_CNT  4
#define AI_BUF_CNT      6

/* 对齐宏 */
#define ALIGN_UP(addr, size)  (((addr) + ((size) - 1U)) & (~((size) - 1U)))

/* 每块大小 (与 sample_venc.c 对齐) */
#define FRAME_BUF_SIZE   ALIGN_UP(ENC_WIDTH * ENC_HEIGHT * 2, 0xFFF)
#define STREAM_BUF_SIZE  ALIGN_UP(ENC_WIDTH * ENC_HEIGHT / 2, 0xFFF)
#define CHN_BUF_SIZE     ALIGN_UP(ENC_WIDTH * ENC_HEIGHT * 3 / 2, 0xFFF)

/* AI 低清旁路配置 */
#define AI_WIDTH   AI_GRAY_MAX_WIDTH
#define AI_HEIGHT  AI_GRAY_MAX_HEIGHT

#ifndef AI_USE_Y_ONLY_FORMAT
#define AI_USE_Y_ONLY_FORMAT 0
#endif

#if AI_USE_Y_ONLY_FORMAT
#define AI_PIXEL_FORMAT  PIXEL_FORMAT_YUV_400
#define AI_CHN_BUF_SIZE  ALIGN_UP(AI_WIDTH * AI_HEIGHT, 0x1000)
#else
#define AI_PIXEL_FORMAT  PIXEL_FORMAT_YUV_SEMIPLANAR_420
#define AI_CHN_BUF_SIZE  ALIGN_UP(AI_WIDTH * AI_HEIGHT * 3 / 2, 0x1000)
#endif

/* 自动退出时间 (秒): 当前已停用，保留宏便于恢复 10 分钟验收模式 */
#define AUTO_EXIT_SEC   600

/* VENC 码流线程配置 */
#define VENC_MAX_PACKS                 MPP_MAX_STREAM_PACKS
#define VENC_GET_STREAM_TIMEOUT_MS     200
#define AI_FRAME_DUMP_TIMEOUT_MS       50

/* RT-Thread 线程配置: 数值越小优先级越高 */
#define STREAM_THREAD_STACK_SIZE       16384
#define STREAM_THREAD_PRIORITY         20
#define STREAM_THREAD_TIMESLICE        10
#define AI_THREAD_STACK_SIZE           16384
#define AI_THREAD_PRIORITY             22
#define AI_THREAD_TIMESLICE            10

/* 通道 ID */
#define VENC_CHN    0
#define VICAP_DEV   VICAP_DEV_ID_0
#define VICAP_CHN   VICAP_CHN_ID_0
#define AI_VICAP_CHN VICAP_CHN_ID_2

/* 管线状态机 — 用于清理时正确跳过未初始化的步骤 */
typedef enum {
    STATUS_IDLE = 0,
    STATUS_VB_INIT,
    STATUS_VENC_CREATED,
    STATUS_VENC_STARTED,
    STATUS_VICAP_INIT,
    STATUS_BINDED,
    STATUS_STREAM_STARTED,
    STATUS_RUNNING,
    STATUS_BUTT
} pipeline_status;

#define THREAD_EXIT_POLL_MS             100
#define STREAM_THREAD_EXIT_TIMEOUT_MS   3000
#define AI_THREAD_EXIT_TIMEOUT_MS       3000

extern volatile sig_atomic_t g_running;
extern volatile sig_atomic_t g_stream_running;
extern pipeline_status g_status;
extern rt_thread_t g_stream_tid;
extern rt_sem_t g_stream_exit_sem;

#define CHECK_RET(ret, func, line)  do { \
    if (ret) \
        printf("[ERROR] ret=0x%x, func=%s line=%d\n", ret, func, line); \
} while(0)

/* 带标签的日志 */
#define LOG(fmt, ...)  printf("[MPP] " fmt "\n", ##__VA_ARGS__)

k_s32 vb_init(void);
k_s32 venc_create_chn(k_u32 chn, k_u32 bitrate);
k_s32 venc_start_chn(k_u32 chn);
k_s32 vicap_try_config(k_vicap_sensor_type sensor_type);
k_s32 vicap_config(k_vicap_sensor_type sensor_type);
k_s32 vi_bind_venc(void);
k_s32 vicap_start(void);
void stream_thread(void *arg);
void stream_thread_stop(void);
void pipeline_cleanup(void);

k_s32 stream_export_init(stream_export_mode mode);
k_s32 stream_export_request_snapshot(const snapshot_request_msg *request);
k_s32 stream_export_submit_venc_stream(k_u32 chn,
                                       const k_venc_stream *stream,
                                       k_bool *release_by_caller);
k_u32 stream_export_get_pending_count(void);
k_s32 stream_export_flush(void);
void stream_export_deinit(void);

k_s32 osd_init(void);
k_s32 osd_set_motion_visible(k_u32 visible, k_u32 duration_ms);
k_s32 osd_poll_auto_hide(void);
void osd_deinit(void);

k_s32 ai_frame_channel_init(void);
k_s32 ai_frame_try_get(ai_gray_frame_view *view, void **ai_frame_handle_out);
k_s32 ai_frame_release(void *ai_frame_handle_ptr);
void ai_frame_channel_deinit(void);

k_s32 motion_adapter_init(void);
k_s32 motion_adapter_process(const ai_gray_frame_view *frame, motion_event_msg *event, k_bool *has_event);
void motion_adapter_deinit(void);

k_s32 ai_motion_thread_start(void);
void ai_motion_thread_stop(void);

#endif /* MPP_PIPELINE_H */
