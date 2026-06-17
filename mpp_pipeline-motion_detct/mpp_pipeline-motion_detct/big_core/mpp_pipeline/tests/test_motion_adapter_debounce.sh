#!/bin/sh
set -eu

root=${1:-.}
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/mpp_pipeline.h" <<'EOF'
#ifndef MPP_PIPELINE_H
#define MPP_PIPELINE_H

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "k_type.h"
#include "mpp_types.h"
#include "motion_detect.h"

#define LOG(fmt, ...) printf("[MPP] " fmt "\n", ##__VA_ARGS__)

k_s32 motion_adapter_init(void);
k_s32 motion_adapter_process(const ai_gray_frame_view *frame,
                             motion_event_msg *event,
                             k_bool *has_event);
void motion_adapter_deinit(void);

#endif /* MPP_PIPELINE_H */
EOF

cat > "$tmpdir/test_motion_adapter_debounce.c" <<'EOF'
#include "mpp_pipeline.h"

static int g_failures;
static motion_detect_result g_results[16];
static unsigned int g_result_count;
static unsigned int g_result_index;

#define EXPECT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", msg); \
            g_failures++; \
        } \
    } while (0)

static void set_result(unsigned int index, k_u32 is_motion, k_u32 score)
{
    g_results[index].is_motion = is_motion;
    g_results[index].motion_score = score;
}

k_s32 motion_detect_process(const ai_gray_frame_view *frame,
                            motion_detect_result *result)
{
    (void)frame;

    if (!result || g_result_index >= g_result_count)
        return -1;

    *result = g_results[g_result_index++];
    return 0;
}

static void run_one(const ai_gray_frame_view *frame,
                    k_bool expected_has_event,
                    const char *message)
{
    motion_event_msg event;
    k_bool has_event = K_FALSE;
    k_s32 ret;

    ret = motion_adapter_process(frame, &event, &has_event);
    EXPECT_TRUE(ret == 0, message);
    EXPECT_TRUE(has_event == expected_has_event, message);
}

int main(void)
{
    k_u8 dummy_luma = 0;
    ai_gray_frame_view frame;
    motion_event_msg event;
    k_bool has_event;
    k_s32 ret;
    unsigned int i;

    memset(&frame, 0, sizeof(frame));
    frame.width = 1;
    frame.height = 1;
    frame.stride = 1;
    frame.y = &dummy_luma;

    for (i = 0; i < 3; ++i)
        set_result(i, K_TRUE, 42);
    for (i = 3; i < 6; ++i)
        set_result(i, K_TRUE, 77);
    for (i = 6; i < 11; ++i)
        set_result(i, K_FALSE, 0);
    for (i = 11; i < 14; ++i)
        set_result(i, K_TRUE, 88);
    g_result_count = 14;

    ret = motion_adapter_init();
    EXPECT_TRUE(ret == 0, "motion_adapter_init should succeed");

    run_one(&frame, K_FALSE, "first motion frame should not trigger event");
    run_one(&frame, K_FALSE, "second motion frame should not trigger event");

    memset(&event, 0, sizeof(event));
    has_event = K_FALSE;
    ret = motion_adapter_process(&frame, &event, &has_event);
    EXPECT_TRUE(ret == 0, "third motion frame should be accepted");
    EXPECT_TRUE(has_event == K_TRUE, "third consecutive motion frame should trigger event");
    EXPECT_TRUE(event.event_id == 1, "first event id should be 1");
    EXPECT_TRUE(event.motion_score == 42, "event should carry detector score");
    EXPECT_TRUE(event.osd_duration_ms == 1000, "event should keep 1000ms OSD duration");

    run_one(&frame, K_FALSE, "latched motion should not trigger event 1");
    run_one(&frame, K_FALSE, "latched motion should not trigger event 2");
    run_one(&frame, K_FALSE, "latched motion should not trigger event 3");

    for (i = 0; i < 5; ++i)
        run_one(&frame, K_FALSE, "still frames should clear latch without event");

    run_one(&frame, K_FALSE, "cooldown first motion frame should not trigger");
    run_one(&frame, K_FALSE, "cooldown second motion frame should not trigger");
    run_one(&frame, K_FALSE, "cooldown suppresses immediate retrigger");

    motion_adapter_deinit();

    if (g_failures) {
        printf("%d motion adapter debounce test(s) failed\n", g_failures);
        return 1;
    }

    printf("motion adapter debounce tests passed\n");
    return 0;
}
EOF

cp "$root/big_core/mpp_pipeline/motion_adapter.c" "$tmpdir/motion_adapter.c"

gcc -Wall -Wextra \
    -I"$tmpdir" \
    -I"$root/big_core/mpp_pipeline" \
    -I/home/ubuntu/k230_sdk/src/big/mpp/include \
    "$tmpdir/motion_adapter.c" \
    "$tmpdir/test_motion_adapter_debounce.c" \
    -o "$tmpdir/test_motion_adapter_debounce"

"$tmpdir/test_motion_adapter_debounce"
