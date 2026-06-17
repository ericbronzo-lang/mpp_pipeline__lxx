#include "mpp_pipeline.h"

static rt_thread_t g_ai_motion_tid = RT_NULL;
static rt_sem_t g_ai_motion_exit_sem = RT_NULL;
static volatile int g_ai_motion_running;
static k_bool g_ai_motion_started;

static k_bool wait_ai_motion_thread_exit(k_u32 timeout_ms)
{
    k_u32 waited_ms = 0;

    if (g_ai_motion_exit_sem == RT_NULL)
        return K_TRUE;

    while (waited_ms < timeout_ms) {
        k_s32 ret = rt_sem_take(g_ai_motion_exit_sem,
                                rt_tick_from_millisecond(THREAD_EXIT_POLL_MS));
        if (ret == RT_EOK)
            return K_TRUE;

        waited_ms += THREAD_EXIT_POLL_MS;
    }

    LOG("AI motion thread exit wait timeout after %ums", timeout_ms);
    return K_FALSE;
}

/*
 * AI motion worker thread.
 *
 * The loop owns the full per-frame lifecycle:
 * 1. ai_frame_try_get() dumps and maps one AI-channel Y plane.
 * 2. motion_adapter_process() runs detection and builds a motion event.
 * 3. ai_frame_release() always returns the dump frame and mmap mapping.
 * 4. osd_set_motion_visible() only queues an OSD request after frame release.
 */
static void ai_motion_thread(void *arg)
{
    k_u32 timeout_count = 0;
    k_u32 frame_count = 0;

    (void)arg;
    LOG("AI motion thread started");

    while (g_ai_motion_running && g_running) {
        k_s32 ret;
        ai_gray_frame_view frame;
        motion_event_msg event;
        k_bool has_event = K_FALSE;
        k_bool trigger_osd = K_FALSE;
        motion_event_msg osd_event;
        void *ai_frame_handle = NULL;

        ret = ai_frame_try_get(&frame, &ai_frame_handle);
        if (ret) {
            if ((timeout_count++ % 100) == 0)
                LOG("AI frame dump timeout/no frame ret=0x%x", ret);
            rt_thread_mdelay(10);
            continue;
        }
        timeout_count = 0;
        frame_count++;

/*
typedef struct {
    k_u32 event_id;
    k_u64 detect_time_ms;
    k_u32 motion_score;
    k_u32 osd_duration_ms;
    k_u32 request_snapshot;
} motion_event_msg;
*/
        ret = motion_adapter_process(&frame, &event, &has_event);
        if (!ret && has_event) {
            LOG("Motion detected: event_id=%u score=%u duration=%ums",
                event.event_id, event.motion_score, event.osd_duration_ms);
            osd_event = event;
            trigger_osd = K_TRUE;
        }

        ret = ai_frame_release(ai_frame_handle);
        if (ret)
            LOG("ai_frame_release failed! ret=0x%x", ret);

        if (trigger_osd) {
            ret = osd_set_motion_visible(1, osd_event.osd_duration_ms);
            if (ret)
                LOG("osd_set_motion_visible failed! ret=0x%x", ret);
        }
    }

    LOG("AI motion thread stopped, frames=%u", frame_count);
    if (g_ai_motion_exit_sem != RT_NULL)
        rt_sem_release(g_ai_motion_exit_sem);
}

k_s32 ai_motion_thread_start(void)
{
    k_s32 ret;

    if (g_ai_motion_started)
        return 0;

    ret = ai_frame_channel_init();
    if (ret)
        return ret;

    ret = motion_adapter_init();
    if (ret) {
        ai_frame_channel_deinit();
        return ret;
    }

    g_ai_motion_exit_sem = rt_sem_create("aidone", 0, RT_IPC_FLAG_FIFO);
    if (g_ai_motion_exit_sem == RT_NULL) {
        LOG("rt_sem_create(aidone) failed!");
        motion_adapter_deinit();
        ai_frame_channel_deinit();
        return -1;
    }

    g_ai_motion_running = 1;
    g_ai_motion_tid = rt_thread_create("ai_motion",
                                       ai_motion_thread,
                                       RT_NULL,
                                       AI_THREAD_STACK_SIZE,
                                       AI_THREAD_PRIORITY,
                                       AI_THREAD_TIMESLICE);
    if (g_ai_motion_tid == RT_NULL) {
        LOG("rt_thread_create(ai_motion) failed!");
        g_ai_motion_running = 0;
        rt_sem_delete(g_ai_motion_exit_sem);
        g_ai_motion_exit_sem = RT_NULL;
        motion_adapter_deinit();
        ai_frame_channel_deinit();
        return -1;
    }

    ret = rt_thread_startup(g_ai_motion_tid);
    if (ret != RT_EOK) {
        LOG("rt_thread_startup(ai_motion) failed! ret=%d", ret);
        g_ai_motion_running = 0;
        rt_thread_delete(g_ai_motion_tid);
        g_ai_motion_tid = RT_NULL;
        rt_sem_delete(g_ai_motion_exit_sem);
        g_ai_motion_exit_sem = RT_NULL;
        motion_adapter_deinit();
        ai_frame_channel_deinit();
        return ret;
    }

    g_ai_motion_started = K_TRUE;
    LOG("AI motion thread create OK");
    return 0;
}

void ai_motion_thread_stop(void)
{
    if (!g_ai_motion_started)
        return;

    g_ai_motion_running = 0;
    if (g_ai_motion_exit_sem != RT_NULL) {
        if (wait_ai_motion_thread_exit(AI_THREAD_EXIT_TIMEOUT_MS)) {
            rt_sem_delete(g_ai_motion_exit_sem);
            g_ai_motion_exit_sem = RT_NULL;
        } else {
            LOG("AI motion thread still stopping; continue cleanup");
        }
    }
    if (g_ai_motion_exit_sem == RT_NULL) {
        g_ai_motion_tid = RT_NULL;
        g_ai_motion_started = K_FALSE;

        motion_adapter_deinit();
        ai_frame_channel_deinit();
        LOG("AI motion thread cleanup OK");
    }
}
