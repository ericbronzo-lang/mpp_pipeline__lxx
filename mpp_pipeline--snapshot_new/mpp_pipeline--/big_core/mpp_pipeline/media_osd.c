#include "mpp_pipeline.h"
#include "motion_detected_osd_512x96_argb8888.h"

/*
 * VENC 2D OSD 叠加模块。
 *
 * 这里不走 VO OSD，而是把 ARGB8888 图片放进 VB/MMZ buffer，
 * 再通过 VENC 2D OSD 参数叠加到编码通道 VENC_CHN 上。AI 运动检测
 * 线程释放 AI frame 后调用 osd_set_motion_visible()，并通过
 * osd_poll_auto_hide() 轮询自动隐藏 deadline。
 */
#define OSD_REGION_INDEX 0
#define OSD_START_X      32
#define OSD_START_Y      32
#define OSD_BUF_SIZE     ALIGN_UP(MOTION_DETECTED_OSD_SIZE, 0x1000)

#define OSD_HIDE_DEADLINE_GRACE_MS  20

/* OSD 图片 buffer 和 VENC 2D 参数状态。 */
static k_bool g_osd_inited = K_FALSE;
static k_bool g_osd_2d_attached = K_FALSE;
static k_vb_blk_handle g_osd_block = VB_INVALID_HANDLE;
static k_u64 g_osd_phys_addr;
static void *g_osd_virt_addr;
static k_venc_2d_osd_attr g_osd_attr;

/* 非 0 表示当前有一次自动隐藏等待执行。 */
static k_u64 g_osd_hide_deadline_ms;

/* 获取单调时间，避免系统时间调整影响 OSD 自动隐藏。 */
static k_u64 osd_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

static k_s32 osd_set_alpha(k_u32 alpha)
{
    k_s32 ret;

    /* 只修改全局透明度，图片 buffer 本身不变。 */
    g_osd_attr.osd_alpha = alpha;
    ret = kd_mpi_venc_set_2d_osd_param(VENC_CHN, OSD_REGION_INDEX, &g_osd_attr);
    if (ret)
        LOG("kd_mpi_venc_set_2d_osd_param alpha=%u failed! ret=0x%x", alpha, ret);

    return ret;
}

static void osd_release_buffer(void)
{
    k_s32 ret;

    /* 释放顺序与申请顺序相反：先 munmap，再 release VB block。 */
    if (g_osd_virt_addr) {
        ret = kd_mpi_sys_munmap(g_osd_virt_addr, OSD_BUF_SIZE);
        CHECK_RET(ret, __func__, __LINE__);
        g_osd_virt_addr = NULL;
    }

    if (g_osd_block != VB_INVALID_HANDLE) {
        ret = kd_mpi_vb_release_block(g_osd_block);
        CHECK_RET(ret, __func__, __LINE__);
        g_osd_block = VB_INVALID_HANDLE;
    }

    g_osd_phys_addr = 0;
}

k_s32 osd_init(void)
{
    k_s32 ret;

    if (g_osd_inited)
        return 0;

    /* 1. 从公共 VB 池申请一块 4K 对齐的 OSD 图像 buffer。 */
    g_osd_block = kd_mpi_vb_get_block(VB_INVALID_POOLID,
                                      OSD_BUF_SIZE,
                                      NULL);
    if (g_osd_block == VB_INVALID_HANDLE) {
        LOG("kd_mpi_vb_get_block(OSD) failed! size=%u",
            (k_u32)OSD_BUF_SIZE);
        return -1;
    }

    g_osd_phys_addr = kd_mpi_vb_handle_to_phyaddr(g_osd_block);
    if (!g_osd_phys_addr) {
        LOG("kd_mpi_vb_handle_to_phyaddr(OSD) failed!");
        osd_release_buffer();
        return -1;
    }
    if (g_osd_phys_addr > 0xffffffffULL) {
        LOG("OSD phys addr out of 32-bit range: 0x%llx",
            (unsigned long long)g_osd_phys_addr);
        osd_release_buffer();
        return -1;
    }

    /* 2. cached mmap 后写入 ARGB8888 图片，再 flush cache 给硬件读取。 */
    g_osd_virt_addr = kd_mpi_sys_mmap_cached(g_osd_phys_addr,
                                             OSD_BUF_SIZE);
    if (!g_osd_virt_addr) {
        LOG("kd_mpi_sys_mmap_cached(OSD) failed!");
        osd_release_buffer();
        return -1;
    }

    memset(g_osd_virt_addr, 0, OSD_BUF_SIZE);
    memcpy(g_osd_virt_addr,
           g_motion_detected_osd_argb8888,
           MOTION_DETECTED_OSD_SIZE);

    ret = kd_mpi_sys_mmz_flush_cache(g_osd_phys_addr,
                                     g_osd_virt_addr,
                                     OSD_BUF_SIZE);
    if (ret) {
        LOG("kd_mpi_sys_mmz_flush_cache(OSD) failed! ret=0x%x", ret);
        osd_release_buffer();
        return ret;
    }

    /* 3. 组装 VENC 2D OSD 参数：初始 osd_alpha=0，等运动触发后再显示。 */
    memset(&g_osd_attr, 0, sizeof(g_osd_attr));
    g_osd_attr.width        = MOTION_DETECTED_OSD_WIDTH;
    g_osd_attr.height       = MOTION_DETECTED_OSD_HEIGHT;
    g_osd_attr.startx       = OSD_START_X;
    g_osd_attr.starty       = OSD_START_Y;
    g_osd_attr.phys_addr[0] = (k_u32)g_osd_phys_addr;
    g_osd_attr.phys_addr[1] = 0;
    g_osd_attr.phys_addr[2] = 0;
    g_osd_attr.bg_alpha     = 0;
    g_osd_attr.osd_alpha    = 0;
    g_osd_attr.video_alpha  = 255;
    g_osd_attr.add_order    = K_VENC_2D_ADD_ORDER_VIDEO_OSD;
    g_osd_attr.bg_color     = 0;
    g_osd_attr.fmt          = K_VENC_2D_OSD_FMT_ARGB8888;

    /* 4. 把 VENC 通道切到 2D OSD 模式，并下发 region 0 的 OSD 参数。 */
    ret = kd_mpi_venc_attach_2d(VENC_CHN);
    if (ret) {
        LOG("kd_mpi_venc_attach_2d failed! ret=0x%x", ret);
        osd_release_buffer();
        return ret;
    }
    g_osd_2d_attached = K_TRUE;

    ret = kd_mpi_venc_set_2d_mode(VENC_CHN, K_VENC_2D_CALC_MODE_OSD);
    if (ret) {
        LOG("kd_mpi_venc_set_2d_mode failed! ret=0x%x", ret);
        kd_mpi_venc_detach_2d(VENC_CHN);
        g_osd_2d_attached = K_FALSE;
        osd_release_buffer();
        return ret;
    }

    ret = kd_mpi_venc_set_2d_osd_param(VENC_CHN, OSD_REGION_INDEX, &g_osd_attr);
    if (ret) {
        LOG("kd_mpi_venc_set_2d_osd_param failed! ret=0x%x", ret);
        kd_mpi_venc_detach_2d(VENC_CHN);
        g_osd_2d_attached = K_FALSE;
        osd_release_buffer();
        return ret;
    }

    g_osd_inited = K_TRUE;
    LOG("VENC 2D OSD init OK: %ux%u ARGB8888 at (%u,%u), phys=0x%llx",
        (k_u32)MOTION_DETECTED_OSD_WIDTH,
        (k_u32)MOTION_DETECTED_OSD_HEIGHT,
        (k_u32)OSD_START_X,
        (k_u32)OSD_START_Y,
        (unsigned long long)g_osd_phys_addr);
    return 0;
}

k_s32 osd_set_motion_visible(k_u32 visible, k_u32 duration_ms)
{
    k_s32 ret;
    k_u32 target_alpha = visible ? 255 : 0;

    if (!g_osd_inited) {
        LOG("OSD motion visible=%u requested before osd_init", visible);
        return -1;
    }

    /*
     * 幂等/续期保护: AI 线程可能连续请求显示或隐藏 OSD。
     * 如果 OSD 已经处于目标显隐状态，就不重复下发 VENC 2D 参数，
     * 只续期自动隐藏 deadline，避免每次请求都调用 set_2d_osd_param()。
     */
    g_osd_hide_deadline_ms = 0;
    if (g_osd_attr.osd_alpha != target_alpha) {
        ret = osd_set_alpha(target_alpha);
        if (ret)
            return ret;
    }

    if (visible && duration_ms > 0) {
        g_osd_hide_deadline_ms = osd_now_ms() + duration_ms;
    }

    return 0;
}

k_s32 osd_poll_auto_hide(void)
{
    k_s32 ret = 0;
    k_u64 now_ms;

    if (g_osd_hide_deadline_ms == 0)
        return 0;

    now_ms = osd_now_ms();
    if (now_ms + OSD_HIDE_DEADLINE_GRACE_MS < g_osd_hide_deadline_ms)
        return 0;

    g_osd_hide_deadline_ms = 0;
    if (g_osd_inited) {
        if (g_osd_attr.osd_alpha != 0) {
            ret = osd_set_alpha(0);
            if (!ret)
                LOG("OSD auto hide alpha=0");
        }
    }

    return ret;
}

void osd_deinit(void)
{
    k_s32 ret;

    if (!g_osd_inited && !g_osd_2d_attached && g_osd_block == VB_INVALID_HANDLE)
        return;

    g_osd_hide_deadline_ms = 0;

    if (g_osd_2d_attached) {
        LOG("VENC 2D OSD detach start");
        ret = kd_mpi_venc_detach_2d(VENC_CHN);
        CHECK_RET(ret, __func__, __LINE__);
        g_osd_2d_attached = K_FALSE;
        LOG("VENC 2D OSD detach done");
    }

    LOG("OSD buffer release start");
    osd_release_buffer();
    LOG("OSD buffer release done");
    memset(&g_osd_attr, 0, sizeof(g_osd_attr));
    g_osd_inited = K_FALSE;
    LOG("VENC 2D OSD deinit OK");
}
