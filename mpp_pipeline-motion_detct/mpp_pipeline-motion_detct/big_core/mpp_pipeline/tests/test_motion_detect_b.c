#include <stdio.h>
#include <string.h>

#include "motion_detect.h"

static int g_failures;

#define EXPECT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            g_failures++; \
        } \
    } while (0)

static void fill_luma(k_u8 *buf, k_u32 stride, k_u32 width, k_u32 height,
                      k_u8 value, k_u8 pad_value)
{
    k_u32 row;

    for (row = 0; row < height; ++row) {
        memset(buf + row * stride, value, width);
        if (stride > width)
            memset(buf + row * stride + width, pad_value, stride - width);
    }
}

int main(void)
{
    enum {
        WIDTH = 16,
        HEIGHT = 8,
        STRIDE = 20
    };
    k_u8 first[STRIDE * HEIGHT];
    k_u8 same_luma_changed_padding[STRIDE * HEIGHT];
    k_u8 changed_luma[STRIDE * HEIGHT];
    motion_detect_result result;
    ai_gray_frame_view frame;
    k_s32 ret;

    memset(&frame, 0, sizeof(frame));
    frame.width = WIDTH;
    frame.height = HEIGHT;
    frame.stride = STRIDE;

    fill_luma(first, STRIDE, WIDTH, HEIGHT, 40, 0x11);
    frame.frame_id = 1;
    frame.y = first;
    result.is_motion = 99;
    result.motion_score = 99;
    ret = motion_detect_process(&frame, &result);
    EXPECT_TRUE(ret == 0, "first frame should seed previous frame");
    EXPECT_TRUE(result.is_motion == 0, "first frame must not trigger motion");
    EXPECT_TRUE(result.motion_score == 0, "first frame score should be zero");

    fill_luma(same_luma_changed_padding, STRIDE, WIDTH, HEIGHT, 40, 0xee);
    frame.frame_id = 2;
    frame.y = same_luma_changed_padding;
    ret = motion_detect_process(&frame, &result);
    EXPECT_TRUE(ret == 0, "same luma frame should be accepted");
    EXPECT_TRUE(result.is_motion == 0, "stride padding must not trigger motion");
    EXPECT_TRUE(result.motion_score == 0, "same luma score should be zero");

    fill_luma(changed_luma, STRIDE, WIDTH, HEIGHT, 90, 0xee);
    frame.frame_id = 3;
    frame.y = changed_luma;
    ret = motion_detect_process(&frame, &result);
    EXPECT_TRUE(ret == 0, "changed luma frame should be accepted");
    EXPECT_TRUE(result.is_motion != 0, "large sampled luma delta should trigger motion");
    EXPECT_TRUE(result.motion_score == 1000, "all sampled pixels changed should score 1000 permille");

    frame.stride = WIDTH - 1;
    ret = motion_detect_process(&frame, &result);
    EXPECT_TRUE(ret != 0, "stride smaller than width should be rejected");

    frame.width = AI_GRAY_MAX_WIDTH + 1;
    frame.height = 1;
    frame.stride = frame.width;
    ret = motion_detect_process(&frame, &result);
    EXPECT_TRUE(ret != 0, "width larger than AI contract should be rejected");

    if (g_failures) {
        printf("%d B motion detect test(s) failed\n", g_failures);
        return 1;
    }

    printf("B motion detect tests passed\n");
    return 0;
}
