#include "mpp_pipeline.h"

/* 单个灰度像素的亮度差超过该阈值，才认为这个采样点发生变化。 */
#define MOTION_PIXEL_DIFF_THRESHOLD 25U
/* motion_score 使用千分比表示，15 约等于 1.5% 的采样点发生变化。 */
#define MOTION_TRIGGER_SCORE        15U
/* 每隔 4 个像素采样一次，降低运动检测线程的 CPU 开销。 */
#define MOTION_SAMPLE_STEP          4U

/* 保存上一帧的 Y 分量，按 width * height 紧密排列，不保留 stride 填充区。 */
static k_u8 *g_prev_frame;
static k_u32 g_prev_width;
static k_u32 g_prev_height;

static void motion_detect_store_frame(const ai_gray_frame_view *frame)
{
    k_u32 row;

    /* 输入帧可能带 stride，这里只拷贝每行有效的 width 字节。 */
    for (row = 0; row < frame->height; ++row) {
        memcpy(g_prev_frame + row * frame->width,
               frame->y + row * frame->stride,
               frame->width);
    }
}

/*
 * 基于帧差法的简单运动检测：
 * 1. 第一帧只作为参考帧保存，不触发运动；
 * 2. 后续帧和上一帧逐点比较 Y 灰度差；
 * 3. 统计变化采样点比例，写入 result->motion_score；
 * 4. 分数超过阈值后，置 result->is_motion。
 */
k_s32 motion_detect_process(const ai_gray_frame_view *frame,
                            motion_detect_result *result)
{
    k_u64 changed_pixels = 0;
    k_u64 sampled_pixels = 0;
    k_u32 y;

    if (!frame || !result || !frame->y)
        return -1;
    if (!frame->width || !frame->height || frame->stride < frame->width)
        return -1;

    /* 默认输出为“无运动”，后续检测到变化再置位。 */
    memset(result, 0, sizeof(*result));

    /* 首帧或分辨率变化时，重新分配上一帧缓存。 */
    if (!g_prev_frame ||
        g_prev_width != frame->width ||
        g_prev_height != frame->height) {
        k_u32 frame_size;

        if (frame->height != 0 && frame->width > ((k_u32)-1) / frame->height)
            return -1;

        frame_size = frame->width * frame->height;
        free(g_prev_frame);
        g_prev_frame = (k_u8 *)malloc(frame_size);
        if (!g_prev_frame) {
            g_prev_width = 0;
            g_prev_height = 0;
            return -1;
        }

        g_prev_width = frame->width;
        g_prev_height = frame->height;
        motion_detect_store_frame(frame);
        return 0;
    }

    /* 抽样比较当前帧和上一帧，避免每个像素都计算带来的额外开销。 */
    for (y = 0; y < frame->height; y += MOTION_SAMPLE_STEP) {
        const k_u8 *cur = frame->y + y * frame->stride;
        const k_u8 *prev = g_prev_frame + y * frame->width;
        k_u32 x;

        for (x = 0; x < frame->width; x += MOTION_SAMPLE_STEP) {
            int diff = (int)cur[x] - (int)prev[x];

            if (diff < 0)
                diff = -diff;
            if ((k_u32)diff >= MOTION_PIXEL_DIFF_THRESHOLD)
                changed_pixels++;
            sampled_pixels++;
        }
    }

    /* motion_score 为变化采样点占比的千分数，范围大致为 0 到 1000。 */
    if (sampled_pixels != 0) {
        result->motion_score = (k_u32)((changed_pixels * 1000ULL) / sampled_pixels);
        result->is_motion = (result->motion_score >= MOTION_TRIGGER_SCORE);
    }

    /* 当前帧成为下一次检测的参考帧。 */
    motion_detect_store_frame(frame);
    return 0;
}
