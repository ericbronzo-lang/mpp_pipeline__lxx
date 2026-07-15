#include <stdlib.h>
#include <string.h>

#include "motion_detect.h"

/* 单个灰度采样点的亮度差超过该阈值，才认为该采样点发生变化。 */
#define MOTION_PIXEL_DIFF_THRESHOLD 10U
/* motion_score 使用千分比表示，15 约等于 1.5% 的采样点发生变化。 */
#define MOTION_TRIGGER_SCORE        8U
/* 每隔 4 个像素采样一次，降低运动检测线程的 CPU 开销。 */
#define MOTION_SAMPLE_STEP          4U


/* 保存上一帧的 Y 分量，按 width * height 紧密排列，不保留 stride 填充区。 */
static k_u8 *g_prev_frame;
static k_u32 g_prev_width;
static k_u32 g_prev_height;

static k_s32 motion_detect_validate_frame(const ai_gray_frame_view *frame)
{
    if (!frame || !frame->y)
        return -1;

    if (!frame->width || !frame->height)
        return -1;

    if (frame->stride < frame->width)
        return -1;

    if (frame->width > AI_GRAY_MAX_WIDTH || frame->height > AI_GRAY_MAX_HEIGHT)
        return -1;

    if (frame->height != 0 && frame->width > ((k_u32)-1) / frame->height)
        return -1;

    return 0;
}

static void motion_detect_store_frame(const ai_gray_frame_view *frame)
{
    k_u32 row;

    for (row = 0; row < frame->height; ++row) {
        memcpy(g_prev_frame + row * frame->width,
               frame->y + row * frame->stride,
               frame->width);
    }
}

/*
 * 基于帧差法的运动检测：首帧只建立参考帧，后续抽样比较当前帧和上一帧
 * 的 Y 分量差值，并用变化采样点千分比填充 motion_score。
 */
k_s32 motion_detect_process(const ai_gray_frame_view *frame,
                            motion_detect_result *result)
{
    k_u64 changed_pixels = 0;
    k_u64 sampled_pixels = 0;
    k_u32 row;

    if (!result)
        return -1;

    memset(result, 0, sizeof(*result));

    if (motion_detect_validate_frame(frame))
        return -1;

    if (!g_prev_frame ||
        g_prev_width != frame->width ||
        g_prev_height != frame->height) {
        k_u32 frame_size = frame->width * frame->height;

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

    for (row = 0; row < frame->height; row += MOTION_SAMPLE_STEP) {
        const k_u8 *cur = frame->y + row * frame->stride;
        const k_u8 *prev = g_prev_frame + row * frame->width;
        k_u32 col;

        for (col = 0; col < frame->width; col += MOTION_SAMPLE_STEP) {
            int diff = (int)cur[col] - (int)prev[col];

            if (diff < 0)
                diff = -diff;
            if ((k_u32)diff >= MOTION_PIXEL_DIFF_THRESHOLD)
                changed_pixels++;
            sampled_pixels++;
        }
    }

    if (sampled_pixels != 0) {
        result->motion_score = (k_u32)((changed_pixels * 1000ULL) / sampled_pixels);
        result->is_motion = result->motion_score >= MOTION_TRIGGER_SCORE;
    }

    motion_detect_store_frame(frame);
    return 0;
}
