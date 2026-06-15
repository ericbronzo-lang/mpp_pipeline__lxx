#include "mpp_pipeline.h"

typedef struct {
    k_video_frame_info frame_info;
    void *virt_addr;
    k_u32 map_size;
    k_u64 frame_id;
} ai_frame_handle;

static k_u64 g_ai_frame_id;

static k_u64 ai_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

static k_u32 ai_frame_stride(const k_video_frame_info *frame_info)
{
    k_u32 stride = frame_info->v_frame.stride[0];

    return stride ? stride : frame_info->v_frame.width;
}

static k_u32 ai_frame_y_size(const k_video_frame_info *frame_info)
{
    k_u32 stride = ai_frame_stride(frame_info);

    return stride * frame_info->v_frame.height;
}

k_s32 ai_frame_channel_init(void)
{
    g_ai_frame_id = 0;
    LOG("AI frame channel ready");
    return 0;
}

k_s32 ai_frame_try_get(ai_gray_frame_view *view, void **handle)
{
    k_s32 ret;
    ai_frame_handle *frame_handle;

    if (!view || !handle)
        return -1;

    memset(view, 0, sizeof(*view));
    *handle = NULL;

    frame_handle = (ai_frame_handle *)calloc(1, sizeof(*frame_handle));
    if (!frame_handle)
        return -1;

    ret = kd_mpi_vicap_dump_frame(VICAP_DEV, AI_VICAP_CHN, VICAP_DUMP_YUV,
                                  &frame_handle->frame_info,
                                  AI_FRAME_DUMP_TIMEOUT_MS);
    if (ret) {
        free(frame_handle);
        return ret;
    }

    frame_handle->map_size = ai_frame_y_size(&frame_handle->frame_info);
    if (frame_handle->map_size == 0) {
        kd_mpi_vicap_dump_release(VICAP_DEV, AI_VICAP_CHN, &frame_handle->frame_info);
        free(frame_handle);
        return -1;
    }

    frame_handle->virt_addr = kd_mpi_sys_mmap(frame_handle->frame_info.v_frame.phys_addr[0],
                                              frame_handle->map_size);
    if (!frame_handle->virt_addr) {
        kd_mpi_vicap_dump_release(VICAP_DEV, AI_VICAP_CHN, &frame_handle->frame_info);
        free(frame_handle);
        return -1;
    }

    frame_handle->frame_id = ++g_ai_frame_id;
    view->frame_id = frame_handle->frame_id;
    view->timestamp_ms = frame_handle->frame_info.v_frame.pts ?
                         frame_handle->frame_info.v_frame.pts : ai_now_ms();
    view->width = frame_handle->frame_info.v_frame.width;
    view->height = frame_handle->frame_info.v_frame.height;
    view->stride = ai_frame_stride(&frame_handle->frame_info);
    view->y = (const k_u8 *)frame_handle->virt_addr;
    *handle = frame_handle;

    return 0;
}

k_s32 ai_frame_release(void *handle)
{
    k_s32 ret = 0;
    ai_frame_handle *frame_handle = (ai_frame_handle *)handle;

    if (!frame_handle)
        return 0;

    if (frame_handle->virt_addr && frame_handle->map_size) {
        ret = kd_mpi_sys_munmap(frame_handle->virt_addr, frame_handle->map_size);
        CHECK_RET(ret, __func__, __LINE__);
    }

    ret = kd_mpi_vicap_dump_release(VICAP_DEV, AI_VICAP_CHN, &frame_handle->frame_info);
    CHECK_RET(ret, __func__, __LINE__);

    free(frame_handle);
    return ret;
}

void ai_frame_channel_deinit(void)
{
    LOG("AI frame channel deinit");
}
