/*主要做“算法结果到管线事件”的适配*/
#include "mpp_pipeline.h"

#define MOTION_CONFIRM_FRAMES   3U
#define MOTION_CLEAR_FRAMES     5U
#define MOTION_COOLDOWN_MS      2000U
#define MOTION_OSD_DURATION_MS  1000U

static k_u32 g_motion_event_id;
static k_u32 g_motion_hit_frames;
static k_u32 g_motion_clear_frames;
static k_bool g_motion_latched;
static k_u64 g_last_motion_event_ms;

static k_u64 motion_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

k_s32 motion_adapter_init(void)
{
    g_motion_event_id = 0;
    g_motion_hit_frames = 0;
    g_motion_clear_frames = 0;
    g_motion_latched = K_FALSE;
    g_last_motion_event_ms = 0;
    LOG("Motion adapter ready");
    return 0;
}

/*
该函数是B侧运动检测与管道控制路径之间的分界点。
它调用motion_detect_process()，
然后将result.is_motion/result.motion_score封装为motion_event_msg，传递给AI线程。
它故意不读取VICAP/VENC缓冲区、释放帧或直接控制OSD；这些责任仍由调用方负责。
 */
k_s32 motion_adapter_process(const ai_gray_frame_view *frame,
                             motion_event_msg *event,
                             k_bool *has_event)
{
    k_s32 ret;
    motion_detect_result result;
    k_u64 now;

    if (!frame || !event || !has_event)
        return -1;

    memset(event, 0, sizeof(*event));
    memset(&result, 0, sizeof(result));
    *has_event = K_FALSE;

    ret = motion_detect_process(frame, &result);
    if (ret) {
        LOG("motion_detect_process failed! ret=0x%x", ret);
        return ret;
    }

    if (result.is_motion) {
        if (g_motion_hit_frames < MOTION_CONFIRM_FRAMES)
            g_motion_hit_frames++;
        g_motion_clear_frames = 0;
    } else {
        g_motion_hit_frames = 0;
        if (g_motion_clear_frames < MOTION_CLEAR_FRAMES)
            g_motion_clear_frames++;
        if (g_motion_clear_frames >= MOTION_CLEAR_FRAMES)
            g_motion_latched = K_FALSE;

        return 0;
    }

    if (g_motion_hit_frames < MOTION_CONFIRM_FRAMES)
        return 0;

    if (g_motion_latched)
        return 0;

    now = motion_now_ms();
    if (g_last_motion_event_ms != 0 &&
        now - g_last_motion_event_ms < MOTION_COOLDOWN_MS)
        return 0;

    event->event_id = ++g_motion_event_id;
    event->detect_time_ms = now;
    event->motion_score = result.motion_score;
    event->osd_duration_ms = MOTION_OSD_DURATION_MS;
    event->request_snapshot = 1;
    *has_event = K_TRUE;
    g_motion_latched = K_TRUE;
    g_last_motion_event_ms = event->detect_time_ms;

    return 0;
}

void motion_adapter_deinit(void)
{
    LOG("Motion adapter deinit");
}
