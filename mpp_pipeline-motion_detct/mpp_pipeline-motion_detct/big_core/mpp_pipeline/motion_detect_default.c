#include <stdlib.h>
#include <string.h>

#include "motion_detect.h"

/*
 * Default weak motion detector used before the real B-side algorithm is linked.
 *
 * The input is the AI low-resolution NV12 Y plane, so each byte is already a
 * luma/grayscale sample. The implementation keeps one private baseline copy,
 * compares the current frame against it, and reports motion_score as the
 * percentage of pixels whose absolute luma delta crosses MOTION_PIXEL_DELTA.
 * A strong motion_detect_process() from the algorithm team overrides this file.
 */
#define MOTION_BASELINE_MAX_SIZE  (AI_GRAY_MAX_WIDTH * AI_GRAY_MAX_HEIGHT)
#define MOTION_PIXEL_DELTA        24U
#define MOTION_RATIO_THRESHOLD    2U

static k_u8 g_motion_baseline[MOTION_BASELINE_MAX_SIZE];
static k_u32 g_motion_baseline_width;
static k_u32 g_motion_baseline_height;
static k_bool g_motion_baseline_valid;

static k_s32 motion_validate_frame(const ai_gray_frame_view *frame)
{
    if (!frame || !frame->y)
        return -1;

    if (!frame->width || !frame->height)
        return -1;

    if (frame->stride < frame->width)
        return -1;

    if (frame->width > AI_GRAY_MAX_WIDTH || frame->height > AI_GRAY_MAX_HEIGHT)
        return -1;

    if (frame->width * frame->height > MOTION_BASELINE_MAX_SIZE)
        return -1;

    return 0;
}

static void motion_copy_baseline(const ai_gray_frame_view *frame)
{
    k_u32 row;

    for (row = 0; row < frame->height; ++row) {
        /* Copy only valid pixels. Bytes after width are alignment padding. */
        memcpy(g_motion_baseline + row * frame->width,
               frame->y + row * frame->stride,
               frame->width);
    }

    g_motion_baseline_width = frame->width;
    g_motion_baseline_height = frame->height;
    g_motion_baseline_valid = K_TRUE;
}

__attribute__((weak)) k_s32 motion_detect_process(const ai_gray_frame_view *frame,
                                                  motion_detect_result *result)
{
    k_u32 row;
    k_u32 changed_pixels = 0;
    k_u32 total_pixels;

    if (!result)
        return -1;

    memset(result, 0, sizeof(*result));

    if (motion_validate_frame(frame))
        return -1;

    if (!g_motion_baseline_valid ||
        g_motion_baseline_width != frame->width ||
        g_motion_baseline_height != frame->height) {
        motion_copy_baseline(frame);
        return 0;
    }

    for (row = 0; row < frame->height; ++row) {
        /* stride is the source row pitch; baseline is tightly packed by width. */
        const k_u8 *cur = frame->y + row * frame->stride;
        const k_u8 *base = g_motion_baseline + row * frame->width;
        k_u32 col;

        for (col = 0; col < frame->width; ++col) {
            k_u32 diff = cur[col] > base[col] ?
                         (k_u32)(cur[col] - base[col]) :
                         (k_u32)(base[col] - cur[col]);

            if (diff >= MOTION_PIXEL_DELTA)
                changed_pixels++;
        }
    }

    total_pixels = frame->width * frame->height;
    result->motion_score = (changed_pixels * 100U) / total_pixels;
    result->is_motion = result->motion_score >= MOTION_RATIO_THRESHOLD;

    motion_copy_baseline(frame);
    return 0;
}
