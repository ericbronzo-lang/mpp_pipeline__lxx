#include "mpp_pipeline.h"

static rt_thread_t g_ai_motion_tid = RT_NULL;
static rt_sem_t g_ai_motion_exit_sem = RT_NULL;
static volatile int g_ai_motion_running;
static k_bool g_ai_motion_started;

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
        void *handle = NULL;

        ret = ai_frame_try_get(&frame, &handle);
        if (ret) {
            if ((timeout_count++ % 100) == 0)
                LOG("AI frame dump timeout/no frame ret=0x%x", ret);
            rt_thread_mdelay(10);
            continue;
        }
        timeout_count = 0;
        frame_count++;

        ret = motion_adapter_process(&frame, &event, &has_event);
        if (!ret && has_event) {
            LOG("Motion detected: event_id=%u score=%u duration=%ums",
                event.event_id, event.motion_score, event.osd_duration_ms);
            osd_set_motion_visible(1, event.osd_duration_ms);
        }

        ret = ai_frame_release(handle);
        if (ret)
            LOG("ai_frame_release failed! ret=0x%x", ret);
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
        k_s32 ret = rt_sem_take(g_ai_motion_exit_sem, RT_WAITING_FOREVER);
        CHECK_RET(ret, __func__, __LINE__);
        rt_sem_delete(g_ai_motion_exit_sem);
        g_ai_motion_exit_sem = RT_NULL;
    }
    g_ai_motion_tid = RT_NULL;
    g_ai_motion_started = K_FALSE;

    motion_adapter_deinit();
    ai_frame_channel_deinit();
    LOG("AI motion thread cleanup OK");
}
