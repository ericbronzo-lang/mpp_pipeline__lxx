#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "motion_detect.h"

/* 根据每帧采样噪声动态选择像素差阈值，兼顾低对比度检测和暗光噪声抑制。 */
#define MOTION_PIXEL_DIFF_MIN       6U
#define MOTION_PIXEL_DIFF_MAX       18U
#define MOTION_NOISE_MARGIN         3U
#define MOTION_NOISE_PERCENTILE     75U
#define MOTION_DIFF_HIST_BINS       256U
/* motion_score 使用千分比表示，8 约等于 0.8% 的采样点发生变化。 */
#define MOTION_TRIGGER_SCORE        8U
/* 每隔 4 个像素采样一次，降低运动检测线程的 CPU 开销。 */
#define MOTION_SAMPLE_STEP          4U
#define MOTION_DIAG_INTERVAL_MS     1000U


/* 保存上一帧的 Y 分量，按 width * height 紧密排列，不保留 stride 填充区。 */
static k_u8 *g_prev_frame;
static k_u32 g_prev_width;
static k_u32 g_prev_height;
static k_u64 g_last_diag_ms;
static const char g_motion_diag_format[] =
    "[MPP] [motion:diag] mean_y=%u global_shift=%d noise_p75=%u "
    "pixel_threshold=%u changed=%llu sampled=%llu score=%u is_motion=%u\n";

static k_u64 motion_detect_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (k_u64)ts.tv_sec * 1000ULL + (k_u64)ts.tv_nsec / 1000000ULL;
}

static k_u32 motion_detect_hist_percentile(const k_u32 *histogram,
                                           k_u64 sample_count,
                                           k_u32 percentile)
{
    k_u64 cumulative = 0;
    k_u64 target;
    k_u32 value;

    if (!histogram || sample_count == 0 || percentile == 0 || percentile > 100)
        return 0;

    target = (sample_count * percentile + 99ULL) / 100ULL;
    for (value = 0; value < MOTION_DIFF_HIST_BINS; ++value) {
        cumulative += histogram[value];
        if (cumulative >= target)
            return value;
    }

    return MOTION_DIFF_HIST_BINS - 1U;
}

static void motion_detect_log_diag(k_u32 mean_y,
                                   int global_shift,
                                   k_u32 noise_p75,
                                   k_u32 pixel_threshold,
                                   k_u64 changed_pixels,
                                   k_u64 sampled_pixels,
                                   const motion_detect_result *result)
{
    k_u64 now_ms = motion_detect_now_ms();

    if (!now_ms || !result)
        return;

    if (g_last_diag_ms != 0 &&
        now_ms - g_last_diag_ms < MOTION_DIAG_INTERVAL_MS)
        return;

    g_last_diag_ms = now_ms;
    printf(g_motion_diag_format,
           mean_y, global_shift, noise_p75, pixel_threshold,
           (unsigned long long)changed_pixels,
           (unsigned long long)sampled_pixels,
           result->motion_score, result->is_motion);
}

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
    k_u32 diff_histogram[MOTION_DIFF_HIST_BINS];
    k_u64 changed_pixels = 0;
    k_u64 sampled_pixels = 0;
    k_u64 current_luma_sum = 0;
    long long signed_diff_sum = 0;
    int global_shift;
    k_u32 mean_y;
    k_u32 noise_p75;
    k_u32 pixel_threshold;
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
        g_last_diag_ms = 0;
        motion_detect_store_frame(frame);
        return 0;
    }

    /*
     * 第一遍采样估算全局亮度偏移。自动曝光造成的整幅同步变亮/变暗
     * 会被该偏移抵消，局部物体变化则保留在补偿后的差值中。
     */
    for (row = 0; row < frame->height; row += MOTION_SAMPLE_STEP) {
        const k_u8 *cur = frame->y + row * frame->stride;
        const k_u8 *prev = g_prev_frame + row * frame->width;
        k_u32 col;

        for (col = 0; col < frame->width; col += MOTION_SAMPLE_STEP) {
            signed_diff_sum += (int)cur[col] - (int)prev[col];
            current_luma_sum += cur[col];
            sampled_pixels++;
        }
    }

    if (sampled_pixels == 0) {
        motion_detect_store_frame(frame);
        return 0;
    }

    global_shift = (int)(signed_diff_sum / (long long)sampled_pixels);
    mean_y = (k_u32)(current_luma_sum / sampled_pixels);

    /*
     * 第二遍建立补偿差值直方图。75% 分位数近似当前帧的噪声底，
     * 加固定余量后限制在 6~18，避免低对比度漏检或暗光噪声误报。
     */
    memset(diff_histogram, 0, sizeof(diff_histogram));
    for (row = 0; row < frame->height; row += MOTION_SAMPLE_STEP) {
        const k_u8 *cur = frame->y + row * frame->stride;
        const k_u8 *prev = g_prev_frame + row * frame->width;
        k_u32 col;

        for (col = 0; col < frame->width; col += MOTION_SAMPLE_STEP) {
            int diff = ((int)cur[col] - (int)prev[col]) - global_shift;

            if (diff < 0)
                diff = -diff;
            if (diff >= (int)MOTION_DIFF_HIST_BINS)
                diff = (int)MOTION_DIFF_HIST_BINS - 1;
            diff_histogram[diff]++;
        }
    }

    noise_p75 = motion_detect_hist_percentile(diff_histogram,
                                              sampled_pixels,
                                              MOTION_NOISE_PERCENTILE);
    pixel_threshold = noise_p75 + MOTION_NOISE_MARGIN;
    if (pixel_threshold < MOTION_PIXEL_DIFF_MIN)
        pixel_threshold = MOTION_PIXEL_DIFF_MIN;
    if (pixel_threshold > MOTION_PIXEL_DIFF_MAX)
        pixel_threshold = MOTION_PIXEL_DIFF_MAX;

    for (row = pixel_threshold; row < MOTION_DIFF_HIST_BINS; ++row)
        changed_pixels += diff_histogram[row];

    result->motion_score = (k_u32)((changed_pixels * 1000ULL) / sampled_pixels);
    result->is_motion = result->motion_score >= MOTION_TRIGGER_SCORE;

    motion_detect_log_diag(mean_y,
                           global_shift,
                           noise_p75,
                           pixel_threshold,
                           changed_pixels,
                           sampled_pixels,
                           result);

    motion_detect_store_frame(frame);
    return 0;
}
