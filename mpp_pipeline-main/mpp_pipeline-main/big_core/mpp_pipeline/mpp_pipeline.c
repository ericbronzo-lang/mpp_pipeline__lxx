/*
 * K230 MPP Pipeline MVP — Week 2 队长任务
 *
 * 单进程多线程验证硬件管线:
 *   Sensor(GC2093) -> VICAP(1080P,NV12) -> [sys_bind] -> VENC(H.265,CBR,15fps)
 *   VICAP(640x480,NV12-Y) -> AI motion thread -> OSD trigger
 *   采集线程: 打印 "Get NALU, Size: xxxx bytes"
 *
 * 验收标准: 15fps 持续打印 NALU 大小, 10 分钟不死机
 */

#include "mpp_pipeline.h"

volatile int g_running = 1;
pipeline_status g_status = STATUS_IDLE;
rt_thread_t g_stream_tid = RT_NULL;
rt_sem_t g_stream_exit_sem = RT_NULL;

/* ================================================================
 * 信号处理 — 安全退出
 * ================================================================ */
static void sig_handler(int signo)
{
    LOG("Received signal %d, stopping...", signo);
    g_running = 0;
}

/* ================================================================
 * main
 * ================================================================ */
int main(void)
{
    k_s32 ret;

    printf("\n");
    printf("========================================\n");
    printf("  K230 MPP pipeline test\n");
    printf("========================================\n\n");

    /* 注册信号处理 */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Step 1: VB 池 */
    ret = vb_init();
    if (ret) goto cleanup;

    /* Step 2: VENC 编码通道 */
    ret = venc_init(VENC_CHN, ENC_BITRATE);
    if (ret) goto cleanup;

    ret = osd_init();
    if (ret) goto cleanup;

    /* Step 3: VICAP 摄像头配置 (GC2093) */
    ret = vicap_config(SENSOR_TYPE);
    if (ret) goto cleanup;

    /* Step 4: 硬件绑定 VI -> VENC */
    ret = vi_bind_venc();
    if (ret) goto cleanup;

    /* Step 5: 启动 VICAP 流 */
    ret = vicap_start();
    if (ret) goto cleanup;

    ret = ai_motion_thread_start();
    if (ret) goto cleanup;

    ret = stream_export_init(STREAM_EXPORT_DATAFIFO);
    // ret = stream_export_init(STREAM_EXPORT_LOCAL_LOG);
    if (ret) goto cleanup;

    g_status = STATUS_RUNNING;

    /* Step 6: 创建 RT-Thread 码流采集线程 */
    g_stream_exit_sem = rt_sem_create("strdone", 0, RT_IPC_FLAG_FIFO);
    if (g_stream_exit_sem == RT_NULL) {
        LOG("rt_sem_create(strdone) failed!");
        ret = -1;
        goto cleanup;
    }

    g_stream_tid = rt_thread_create("mpp_str",
                                    stream_thread,
                                    (void *)(k_u64)VENC_CHN,
                                    STREAM_THREAD_STACK_SIZE,
                                    STREAM_THREAD_PRIORITY,
                                    STREAM_THREAD_TIMESLICE);
    if (g_stream_tid == RT_NULL) {
        LOG("rt_thread_create(mpp_str) failed!");
        rt_sem_delete(g_stream_exit_sem);
        g_stream_exit_sem = RT_NULL;
        ret = -1;
        goto cleanup;
    }

    ret = rt_thread_startup(g_stream_tid);
    if (ret != RT_EOK) {
        LOG("rt_thread_startup(mpp_str) failed! ret=%d", ret);
        rt_thread_delete(g_stream_tid);
        g_stream_tid = RT_NULL;
        rt_sem_delete(g_stream_exit_sem);
        g_stream_exit_sem = RT_NULL;
        goto cleanup;
    }

    LOG("Pipeline running");

    /* 主循环: 等待退出信号或超时 */
    {
        time_t start = time(NULL);
        while (g_running && difftime(time(NULL), start) < AUTO_EXIT_SEC) {
            rt_thread_mdelay(1000);
        }
        if (g_running) {
            LOG("Auto-exit timeout reached");
            g_running = 0;
        }
    }

    /* 等待 RT 采集线程自然退出 */
    if (g_stream_exit_sem != RT_NULL) {
        ret = rt_sem_take(g_stream_exit_sem, RT_WAITING_FOREVER);
        CHECK_RET(ret, __func__, __LINE__);
        rt_sem_delete(g_stream_exit_sem);
        g_stream_exit_sem = RT_NULL;
    }
    g_stream_tid = RT_NULL;

cleanup:
    pipeline_cleanup();

    if (ret == 0)
        LOG("Pipeline test PASSED.");
    else
        LOG("Pipeline test FAILED (ret=0x%x). Check error codes above.", ret);

    return ret;
}
