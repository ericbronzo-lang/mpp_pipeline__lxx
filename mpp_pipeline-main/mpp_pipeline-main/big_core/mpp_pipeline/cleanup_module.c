#include "mpp_pipeline.h"

/* ================================================================
 * 清理函数 — 逆序释放资源
 *
 * 关键: 顺序错了会死机!
 * 使用状态机保证中间某步失败时也能正确跳过
 * ================================================================ */
void pipeline_cleanup(void)
{
    k_s32 ret;

    LOG("Cleanup started");
    ai_motion_thread_stop();
    stream_export_deinit();
    osd_deinit();

    /* 5. 停 VICAP 流 */
    if (g_status >= STATUS_STREAM_STARTED) {
        ret = kd_mpi_vicap_stop_stream(VICAP_DEV);
        CHECK_RET(ret, __func__, __LINE__);
    }

    /* 4. 解绑 */
    if (g_status >= STATUS_BINDED) {
        k_mpp_chn vi_mpp_chn  = { .mod_id = K_ID_VI,   .dev_id = VICAP_CHN, .chn_id = VICAP_CHN };
        k_mpp_chn venc_mpp_chn = { .mod_id = K_ID_VENC, .dev_id = 0,         .chn_id = VENC_CHN };
        ret = kd_mpi_sys_unbind(&vi_mpp_chn, &venc_mpp_chn);
        CHECK_RET(ret, __func__, __LINE__);
    }

    /* 3. 反初始化 VICAP */
    if (g_status >= STATUS_VICAP_INIT) {
        ret = kd_mpi_vicap_deinit(VICAP_DEV);
        CHECK_RET(ret, __func__, __LINE__);
    }

    /* 2. 停 + 销毁 VENC */
    if (g_status >= STATUS_VENC_STARTED) {
        ret = kd_mpi_venc_stop_chn(VENC_CHN);
        CHECK_RET(ret, __func__, __LINE__);
    }
    if (g_status >= STATUS_VENC_CREATED) {
        ret = kd_mpi_venc_destroy_chn(VENC_CHN);
        CHECK_RET(ret, __func__, __LINE__);
    }

    /* 1. 关闭 VENC fd + 退出 VB */
    if (g_status >= STATUS_VENC_CREATED) {
        kd_mpi_venc_close_fd();
    }
    if (g_status >= STATUS_VB_INIT) {
        ret = kd_mpi_vb_exit();
        CHECK_RET(ret, __func__, __LINE__);
    }

    LOG("Cleanup complete");
}
