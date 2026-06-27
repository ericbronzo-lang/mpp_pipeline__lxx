#ifndef MOTION_DETECT_H
#define MOTION_DETECT_H

#include "mpp_types.h"

/*
 * B side algorithm contract:
 * - frame->y points to the readable Y plane of the AI NV12 low-resolution frame.
 * - Access each row with frame->stride; stride is not guaranteed to equal width.
 * - The implementation only fills result and must not control OSD, VICAP, VENC,
 *   RT-Thread objects, or frame release.
 * - A strong definition of this function overrides the default weak example.
 */
k_s32 motion_detect_process(const ai_gray_frame_view *frame,
                            motion_detect_result *result);

#endif /* MOTION_DETECT_H */
