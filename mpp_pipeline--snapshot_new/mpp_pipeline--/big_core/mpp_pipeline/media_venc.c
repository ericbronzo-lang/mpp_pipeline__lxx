#include "mpp_pipeline.h"

/* ================================================================
 * Step 2: VENC 编码通道配置
 *
 * H.265 Main Profile, CBR 4000kbps
 *
 * 注意: src_frame_rate/dst_frame_rate 是 VENC 码率控制/目标帧率参数；
 * 当前 VICAP->VENC 硬件绑定链路是否实际降帧，要以 stream_thread 统计为准。
 * ================================================================ */
/**
 * @brief 创建VENC（视频编码）通道
 *
 * 该函数只负责创建H.265编码通道，配置为CBR码率控制模式。
 * src_frame_rate/dst_frame_rate 按官方定义分别表示VENC通道输入帧率和目标帧率，
 * 但它们不是VICAP侧的硬件丢帧配置。
 *
 * @param chn 通道号
 * @param bitrate 目标码率（kbps）
 * @return k_s32 成功返回0，失败返回错误码
 */
k_s32 venc_create_chn(k_u32 chn, k_u32 bitrate)
{
    k_s32 ret;                                  // 返回值，用于存储API调用结果
    k_venc_chn_attr attr;                       // 视频编码通道属性结构体

    memset(&attr, 0, sizeof(attr));             // 清零编码通道属性结构体，确保所有字段初始化为0

    /* 编码器属性 */
    attr.venc_attr.type            = K_PT_H265;   // 设置编码类型为H.265
    attr.venc_attr.profile         = VENC_PROFILE_H265_MAIN;  // 设置编码配置为Main Profile
    attr.venc_attr.pic_width       = ENC_WIDTH;   // 设置编码图片宽度
    attr.venc_attr.pic_height      = ENC_HEIGHT;  // 设置编码图片高度
    attr.venc_attr.stream_buf_size = STREAM_BUF_SIZE;  // 设置码流缓冲区大小
    attr.venc_attr.stream_buf_cnt  = OUTPUT_BUF_CNT;   // 设置码流缓冲区数量

    /* 码率控制: CBR */
    attr.rc_attr.rc_mode           = K_VENC_RC_MODE_CBR;  // 设置码率控制模式为CBR（恒定比特率）
    attr.rc_attr.cbr.src_frame_rate = SRC_FPS;    // VENC通道输入帧率参数，当前VICAP实际约30fps
    attr.rc_attr.cbr.dst_frame_rate = DST_FPS;    // VENC目标输出帧率参数；不负责配置VICAP硬件丢帧
    attr.rc_attr.cbr.bit_rate       = bitrate;    // 设置目标码率为指定值
    attr.rc_attr.cbr.gop            = VENC_GOP;  /* 跟随 DST_FPS，约 1 秒一个 I 帧 */

    ret = kd_mpi_venc_create_chn(chn, &attr);     // 创建VENC通道，使用指定的通道号和属性
    if (ret) {                                  // 检查通道创建是否成功
        LOG("kd_mpi_venc_create_chn failed! ret=0x%x", ret);  // 记录创建失败的日志
        return ret;                             // 返回错误码
    }
    g_status = STATUS_VENC_CREATED;             // 更新全局状态为VENC已创建

    LOG("VENC chn=%u create OK", chn);         // 记录创建成功的日志
    return 0;                                   // 返回成功状态
}

/* ================================================================
 * 启动 VENC 编码通道
 * ================================================================ */
k_s32 venc_start_chn(k_u32 chn)
{
    k_s32 ret;

    ret = kd_mpi_venc_start_chn(chn);           // 启动VENC通道
    if (ret) {                                  // 检查通道启动是否成功
        LOG("kd_mpi_venc_start_chn failed! ret=0x%x", ret);  // 记录启动失败的日志
        return ret;                             // 返回错误码
    }
    g_status = STATUS_VENC_STARTED;             // 更新全局状态为VENC已启动

    LOG("VENC chn=%u start OK", chn);          // 记录启动成功的日志
    return 0;                                   // 返回成功状态
}

/* ================================================================
 * Step 5: 码流采集线程
 *
 * 从 VENC 获取编码后的 NALU, 打印大小
 * 实际输出帧率以这里的统计为准；当前硬件绑定链路实测约 30fps。
 * ================================================================ */
/**
 * @brief 码流处理线程函数
 * 
 * 该函数在一个独立线程中运行，持续从VENC模块获取编码后的NALU（网络抽象层单元），
 * 并统计帧数、字节数等信息，定期输出性能统计信息
 * 
 * @param arg 传入的参数，转换为通道号
 * @return void* 线程返回值（始终为NULL）
 */
void stream_thread(void *arg)
{
    k_u32 chn = (k_u32)(k_u64)arg;              // 将传入参数转换为通道号
    k_venc_stream output;                        // 存储编码后的码流数据
    k_venc_pack packs[VENC_MAX_PACKS];           // SDK get_stream() 的临时 pack 描述缓冲，不承载码流数据
    k_s32 ret;                                  // 返回值，用于存储API调用结果
    k_u32 frame_count = 0;                      // 总帧计数器，统计接收到的所有非头部帧
    k_u32 total_bytes = 0;                      // 总字节数计数器
    time_t start_time, last_log;                // 开始时间戳和上次日志记录时间戳
    k_u32 interval_frames = 0;                  // 区间帧计数器，用于计算区间内的帧率
    k_u32 query_fail_count = 0;                 // 查询失败计数，用于限制日志频率
    k_u32 get_fail_count = 0;                   // get_stream失败计数，用于限制日志频率

    LOG("Stream thread started");               // 记录线程启动日志
    start_time = last_log = time(NULL);         // 记录开始时间和上次日志时间
    memset(&output, 0, sizeof(output));
    output.pack = packs;

    while (g_stream_running) {                  // cleanup 明确停止前持续 drain VENC
        k_venc_chn_status status;               // VENC通道状态结构体
        k_bool release_by_caller = K_TRUE;       // 导出层返回的释放责任

        /* 查询当前有多少个 pack */
        ret = kd_mpi_venc_query_status(chn, &status);  // 查询VENC通道的状态
        if (ret) {                              // 检查查询是否成功
            if (!g_stream_running)
                break;
            if ((query_fail_count++ % 25) == 0)
                LOG("kd_mpi_venc_query_status failed! ret=0x%x", ret);  // 记录错误日志
            rt_thread_mdelay(50);               // 休眠50毫秒后重试
            continue;                           // 继续下一轮循环
        }
        query_fail_count = 0;

        memset(packs, 0, sizeof(packs));
        memset(&output, 0, sizeof(output));
        output.pack = packs;
        output.pack_cnt = status.cur_packs;      // 设置包数量
        if (output.pack_cnt == 0)
            output.pack_cnt = 1;
        if (output.pack_cnt > VENC_MAX_PACKS)
            output.pack_cnt = VENC_MAX_PACKS;

        /* 有限等待编码完成，保证 Ctrl+C 和自动退出能回收线程 */
        ret = kd_mpi_venc_get_stream(chn, &output, VENC_GET_STREAM_TIMEOUT_MS);
        if (ret) {                              // 检查获取码流是否成功
            if (!g_stream_running)               // 如果 cleanup 要求退出
                break;                           // 跳出循环结束线程
            {
                k_s32 flush_ret = stream_export_flush();
                if (flush_ret && ((get_fail_count % 25) == 0))
                    LOG("stream_export_flush failed! ret=0x%x", flush_ret);
            }
            if ((get_fail_count++ % 25) == 0)
                LOG("kd_mpi_venc_get_stream timeout/no stream ret=0x%x", ret);
            continue;                            // 暂时无流时继续等待
        }
        get_fail_count = 0;

        if (!g_stream_running) {
            ret = kd_mpi_venc_release_stream(chn, &output);
            if (ret)
                LOG("kd_mpi_venc_release_stream failed! ret=0x%x", ret);
            break;
        }

        /* 遍历所有 pack, 只统计非 header 的数据帧 */
        for (k_u32 i = 0; i < output.pack_cnt; i++) {  // 遍历所有编码包
            if (output.pack[i].type != K_VENC_HEADER) {  // 检查是否为非头部帧（即实际数据帧）
                frame_count++;                  // 增加总帧计数
                interval_frames++;              // 增加区间帧计数
                total_bytes += output.pack[i].len;  // 累加数据长度
            }
        }

        ret = stream_export_submit_venc_stream(chn, &output, &release_by_caller);
        if (ret)
            LOG("stream_export_submit_venc_stream failed! ret=0x%x", ret);

        if (release_by_caller) {
            ret = kd_mpi_venc_release_stream(chn, &output);  // 释放已获取的码流资源
            if (ret) {                              // 检查释放操作是否成功
                LOG("kd_mpi_venc_release_stream failed! ret=0x%x", ret);  // 记录错误日志
            }
        }

        if (!g_stream_running)
            break;

        /* STREAM_EXPORT_LOCAL_LOG 下采集线程可能持续满速运行；主动让出调度，保证 Ctrl+C 信号路径及时执行。 */
        rt_thread_mdelay(1);

        /* 每 150 帧统计一次帧率；15fps 时约 10 秒，30fps 时约 5 秒 */
        if (interval_frames >= 150) {           // 当区间帧数达到150时进行统计
            time_t now = time(NULL);            // 获取当前时间
            double elapsed = difftime(now, last_log);  // 计算时间间隔
            double fps = (elapsed > 0) ? interval_frames / elapsed : 0;  // 计算帧率
            LOG("=== Stats: %u frames in %.1fs, %.1f fps, total %u bytes ===",
                interval_frames, elapsed, fps, total_bytes);  // 输出统计信息
            interval_frames = 0;                // 重置区间帧计数器
            last_log = now;                     // 更新上次日志时间
        }
    }

    time_t end_time = time(NULL);               // 获取结束时间
    double total_elapsed = difftime(end_time, start_time);  // 计算总耗时
    LOG("Stream thread exit. Total: %u frames, %u bytes in %.1fs (avg %.1f fps)",
        frame_count, total_bytes, total_elapsed,  // 输出总计信息：总帧数、总字节数、总时间
        total_elapsed > 0 ? frame_count / total_elapsed : 0);  // 计算平均帧率

    if (g_stream_exit_sem != RT_NULL)
        rt_sem_release(g_stream_exit_sem);
}

static k_bool wait_stream_thread_exit(k_u32 timeout_ms)
{
    k_u32 waited_ms = 0;

    if (g_stream_exit_sem == RT_NULL)
        return K_TRUE;

    while (waited_ms < timeout_ms) {
        k_s32 ret = rt_sem_take(g_stream_exit_sem,
                                rt_tick_from_millisecond(THREAD_EXIT_POLL_MS));
        if (ret == RT_EOK)
            return K_TRUE;

        waited_ms += THREAD_EXIT_POLL_MS;
    }

    LOG("Stream thread exit wait timeout after %ums", timeout_ms);
    return K_FALSE;
}

void stream_thread_stop(void)
{
    if (g_stream_tid == RT_NULL && g_stream_exit_sem == RT_NULL)
        return;

    LOG("stream_thread_stop start");
    g_stream_running = 0;

    if (g_stream_exit_sem != RT_NULL) {
        if (wait_stream_thread_exit(STREAM_THREAD_EXIT_TIMEOUT_MS)) {
            rt_sem_delete(g_stream_exit_sem);
            g_stream_exit_sem = RT_NULL;
            g_stream_tid = RT_NULL;
            LOG("stream_thread_stop done");
        } else {
            LOG("stream_thread_stop timeout; continue cleanup");
        }
    } else {
        g_stream_tid = RT_NULL;
        LOG("stream_thread_stop done");
    }
}
