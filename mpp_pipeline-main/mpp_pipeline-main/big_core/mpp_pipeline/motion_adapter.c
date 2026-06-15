#include "mpp_pipeline.h"

/* 连续检测到多少帧运动后，才确认触发事件。 */
#define MOTION_CONFIRM_FRAMES  3U
/* 连续检测到多少帧静止后，才认为上一段运动已经结束。 */
#define MOTION_CLEAR_FRAMES    5U
/* 两次 OSD 事件之间的最小间隔，避免短时间反复显示。 */
#define MOTION_COOLDOWN_MS     2000U
/* 单次 OSD 显示持续时间。 */
#define MOTION_OSD_DURATION_MS 1000U

static k_u32 g_motion_event_id;
/* 连续运动帧计数。 */
static k_u32 g_motion_hit_frames;
/* 连续静止帧计数。 */
static k_u32 g_motion_clear_frames;
/* 一段运动触发过事件后先锁住，等静止足够多帧再释放。 */
static k_bool g_motion_latched;
/* 上一次真正发送运动事件的时间。 */
static k_u64 g_last_motion_event_ms;

static k_u64 motion_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

__attribute__((weak)) k_s32 motion_detect_process(const ai_gray_frame_view *frame,
                                                  motion_detect_result *result)
{
    if (!frame || !result)
        return -1;

    memset(result, 0, sizeof(*result));
    return 0;
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

    /*
     * 去抖第一层：连续帧确认。
     * 只有连续多帧检测到运动，才认为这是一段有效运动，避免单帧噪声触发 OSD。
     */
    if (result.is_motion) {
        if (g_motion_hit_frames < MOTION_CONFIRM_FRAMES)
            g_motion_hit_frames++;
        g_motion_clear_frames = 0;
    } else {
        g_motion_hit_frames = 0;
        if (g_motion_clear_frames < MOTION_CLEAR_FRAMES)
            g_motion_clear_frames++;

        /*
         * 去抖第二层：释放条件。
         * 只有连续多帧没有运动，才解除锁存，允许下一段运动重新触发事件。
         */
        if (g_motion_clear_frames >= MOTION_CLEAR_FRAMES)
            g_motion_latched = K_FALSE;

        return 0;
    }

    if (g_motion_hit_frames < MOTION_CONFIRM_FRAMES)
        return 0;

    /*
     * 去抖第三层：事件锁存。
     * 一段持续运动只发送一次事件，避免每一帧都调用 osd_set_motion_visible()。
     */
    if (g_motion_latched)
        return 0;

    now = motion_now_ms();

    /*
     * 冷却时间：即使刚释放锁存，也不要在很短时间内再次发送 OSD 事件。
     */
    if (g_last_motion_event_ms != 0 &&
        now - g_last_motion_event_ms < MOTION_COOLDOWN_MS)
        return 0;

    event->event_id = ++g_motion_event_id;
    event->detect_time_ms = now;
    event->motion_score = result.motion_score;
    event->osd_duration_ms = MOTION_OSD_DURATION_MS;
    event->request_snapshot = 0;
    *has_event = K_TRUE;
    g_motion_latched = K_TRUE;
    g_last_motion_event_ms = event->detect_time_ms;

    return 0;
}

void motion_adapter_deinit(void)
{
    LOG("Motion adapter deinit");
}
