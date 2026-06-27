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

//v_frame.stride 是视频帧在内存里“一行数据占多少字节”，也叫行跨度 stride >= width
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

/*
 * Try to fetch one low-resolution AI frame from VICAP channel 2.
 *
 * On success, view describes only the NV12 Y plane, which the motion algorithm
 * treats as grayscale input. ai_frame_handle_out owns the SDK dump frame and
 * mmap mapping; the caller must pass it to ai_frame_release() exactly once.
 * view->stride is copied from the SDK frame info and must be used when walking
 * rows because DMA alignment can make stride larger than width.
 */
k_s32 ai_frame_try_get(ai_gray_frame_view *view, void **ai_frame_handle_out)
{
    k_s32 ret;
    ai_frame_handle *frame_handle;

    if (!view || !ai_frame_handle_out)
        return -1;

    memset(view, 0, sizeof(*view));
    *ai_frame_handle_out = NULL;

    //成功时返回一个指向分配好的内存的 void * 指针；失败则返回 NULL
    frame_handle = (ai_frame_handle *)calloc(1, sizeof(*frame_handle));
    if (!frame_handle)
        return -1;

/**
 * @brief dump frame from the VICAP 从VICAP中提取帧
 * @param [in] dev_num device number
 * @param [in] chn_num channel number
 * @param [in] foramt  dump data format
 * @param [out] vf_info Video frame information obtained
 * @param [in] milli_sec  timeout value
 */
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

/**
 * @brief Maps the memory storage address no cache 
 *          把 MPP/VICAP 给的“物理地址”映射成当前程序 CPU 可以直接访问的“虚拟地址”
 * @param [in] phy_addr Start address of the memory to be mapped 要映射的内存起始地址
 * @param [in] size Number of mapped bytes 映射的字节数 
 * - 只有由底层使用MMZ请求的物理内存区域才能被映射。
 * - 对于VB内存区域，映射的字节数不能超过VB POOL的大小。
 */
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
    *ai_frame_handle_out = frame_handle;

    return 0;
}

k_s32 ai_frame_release(void *ai_frame_handle_ptr)
{
    k_s32 ret = 0;
    ai_frame_handle *frame_handle = (ai_frame_handle *)ai_frame_handle_ptr;

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
