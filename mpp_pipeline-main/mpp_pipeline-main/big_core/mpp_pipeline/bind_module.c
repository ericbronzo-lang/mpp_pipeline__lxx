#include "mpp_pipeline.h"

/* ================================================================
 * Step 4: 硬件绑定 VI → VENC
 *
 * 绑定后 VI 采集的帧自动送入 VENC, CPU 零拷贝
 * ================================================================ */
/**
 * @brief 绑定VI模块到VENC模块
 * 
 * 该函数将视频输入模块(VI)与视频编码模块(VENC)进行绑定，
 * 实现视频数据从VI模块直接传输到VENC模块进行编码，无需CPU拷贝
 * 
 * @return k_s32 成功返回0，失败返回错误码
 */
k_s32 vi_bind_venc(void)
{
    k_s32 ret;                                  // 返回值，用于存储API调用结果
    k_mpp_chn vi_mpp_chn;                       // VI模块的通道信息结构体
    k_mpp_chn venc_mpp_chn;                     // VENC模块的通道信息结构体

    vi_mpp_chn.mod_id = K_ID_VI;                // 设置VI模块ID
    vi_mpp_chn.dev_id = VICAP_CHN;              // 设置VI设备ID为VICAP通道号
    vi_mpp_chn.chn_id = VICAP_CHN;              // 设置VI通道ID为VICAP通道号

    venc_mpp_chn.mod_id = K_ID_VENC;            // 设置VENC模块ID
    venc_mpp_chn.dev_id = 0;                    // 设置VENC设备ID为0
    venc_mpp_chn.chn_id = VENC_CHN;             // 设置VENC通道ID为VENC通道号

    ret = kd_mpi_sys_bind(&vi_mpp_chn, &venc_mpp_chn);  // 执行系统绑定，连接VI和VENC模块
    if (ret) {                                  // 检查绑定操作是否成功
        LOG("kd_mpi_sys_bind failed! ret=0x%x", ret);  // 记录绑定失败的日志
        return ret;                             // 返回错误码
    }
    g_status = STATUS_BINDED;                   // 更新全局状态为已绑定

    LOG("VI->VENC bind OK");                  // 记录绑定成功的日志
    return 0;                                   // 返回成功状态
}
