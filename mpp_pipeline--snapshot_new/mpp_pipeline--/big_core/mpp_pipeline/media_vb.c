#include "mpp_pipeline.h"

/* ================================================================
 * Step 1: VB 内存池初始化
 *
 * 内存计算推导:
 *   1080P NV12 单帧含 padding:
 *     FRAME_BUF_SIZE = (1920*1080*2 + 0xFFF) & ~0xFFF = 0x3F5000 ≈ 4.0 MB
 *   编码输出流 buffer:
 *     STREAM_BUF_SIZE = (1920*1080/2 + 0xFFF) & ~0xFFF = 0xFD800 ≈ 1.0 MB
 *   每个 channel 的帧 buffer:
 *     CHN_BUF_SIZE = (1920*1080*3/2 + 0xFFF) & ~0xFFF = 0x2F8000 ≈ 3.0 MB
 *   AI 低清旁路 buffer:
 *     AI_CHN_BUF_SIZE = ALIGN_UP(640*480*3/2, 0x1000) = 0x71000 ≈ 450 KB
 *
 *   总内存: 6*4MB + 15*1MB + 6*450KB ≈ 41.3 MB (K230 有 512MB DDR)
 * ================================================================ */
k_s32 vb_init(void)
{
    k_s32 ret;
    k_vb_config config;

    memset(&config, 0, sizeof(config));
    config.max_pool_cnt = 3;

    /* Pool 0: 输入帧 buffer */
    config.comm_pool[0].blk_cnt  = INPUT_BUF_CNT;
    config.comm_pool[0].blk_size = FRAME_BUF_SIZE;
    config.comm_pool[0].mode     = VB_REMAP_MODE_NOCACHE;

    /* Pool 1: 编码输出码流 buffer */
    config.comm_pool[1].blk_cnt  = OUTPUT_BUF_CNT;
    config.comm_pool[1].blk_size = STREAM_BUF_SIZE;
    config.comm_pool[1].mode     = VB_REMAP_MODE_NOCACHE;

    /* Pool 2: AI 低清旁路帧 buffer */
    config.comm_pool[2].blk_cnt  = AI_BUF_CNT;
    config.comm_pool[2].blk_size = AI_CHN_BUF_SIZE;
    config.comm_pool[2].mode     = VB_REMAP_MODE_NOCACHE;

    ret = kd_mpi_vb_set_config(&config);
    if (ret) {
        LOG("kd_mpi_vb_set_config failed! ret=0x%x", ret);
        return ret;
    }

    ret = kd_mpi_vb_init();
    if (ret) {
        LOG("kd_mpi_vb_init failed! ret=0x%x", ret);
        return ret;
    }

    LOG("VB pool init OK");
    g_status = STATUS_VB_INIT;
    return 0;
}
