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
    LOG("Cleanup flow: producer-stop-before-stream-stop");
    ai_motion_thread_stop();

    /* 5. 停 VICAP 流 */
    if (g_status >= STATUS_STREAM_STARTED) {
        LOG("vicap_stop_stream start");
        ret = kd_mpi_vicap_stop_stream(VICAP_DEV);
        CHECK_RET(ret, __func__, __LINE__);
        LOG("vicap_stop_stream done ret=0x%x", ret);
    }

    /* 4. 解绑 */
    if (g_status >= STATUS_BINDED) {
        k_mpp_chn vi_mpp_chn  = { .mod_id = K_ID_VI,   .dev_id = VICAP_CHN, .chn_id = VICAP_CHN };
        k_mpp_chn venc_mpp_chn = { .mod_id = K_ID_VENC, .dev_id = 0,         .chn_id = VENC_CHN };
        LOG("sys_unbind VI->VENC start");
        ret = kd_mpi_sys_unbind(&vi_mpp_chn, &venc_mpp_chn);
        CHECK_RET(ret, __func__, __LINE__);
        LOG("sys_unbind VI->VENC done ret=0x%x", ret);
    }

    /* 3. 反初始化 VICAP */
    if (g_status >= STATUS_VICAP_INIT) {
        LOG("vicap_deinit start");
        ret = kd_mpi_vicap_deinit(VICAP_DEV);
        CHECK_RET(ret, __func__, __LINE__);
        LOG("vicap_deinit done ret=0x%x", ret);
    }

    stream_thread_stop();

    LOG("stream_export_deinit start");
    stream_export_deinit();
    LOG("stream_export_deinit done");

    /*
     * 2. 停 VENC 后再 detach 2D OSD，最后销毁 VENC。
     * VENC 2D OSD 在通道 create 后、start 前 attach；清理时需要先停止
     * VICAP/VENC 数据流，避免 detach_2d 在通道运行中阻塞。
     */
    if (g_status >= STATUS_VENC_STARTED) {
        LOG("venc_stop start");
        ret = kd_mpi_venc_stop_chn(VENC_CHN);
        CHECK_RET(ret, __func__, __LINE__);
        LOG("venc_stop done ret=0x%x", ret);
    }
    LOG("osd_deinit start");
    osd_deinit();
    LOG("osd_deinit done");
    if (g_status >= STATUS_VENC_CREATED) {
        LOG("venc_destroy start");
        ret = kd_mpi_venc_destroy_chn(VENC_CHN);
        CHECK_RET(ret, __func__, __LINE__);
        LOG("venc_destroy done ret=0x%x", ret);
    }

    /* 1. 关闭 VENC fd + 退出 VB */
    if (g_status >= STATUS_VENC_CREATED) {
        LOG("venc_close_fd start");
        kd_mpi_venc_close_fd();
        LOG("venc_close_fd done");
    }
    if (g_status >= STATUS_VB_INIT) {
        LOG("vb_exit start");
        ret = kd_mpi_vb_exit();
        CHECK_RET(ret, __func__, __LINE__);
        LOG("vb_exit done ret=0x%x", ret);
    }

    LOG("Cleanup complete");
}
