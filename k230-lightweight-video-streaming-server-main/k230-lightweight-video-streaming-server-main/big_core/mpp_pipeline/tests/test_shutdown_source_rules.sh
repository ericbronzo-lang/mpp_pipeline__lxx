#!/bin/sh
set -eu

root=${1:-.}

handler=$(sed -n '/static void sig_handler/,/^}/p' "$root/big_core/mpp_pipeline/mpp_pipeline.c")
if printf '%s\n' "$handler" | grep -Eq 'LOG\(|printf\('; then
    echo "sig_handler must not call LOG/printf"
    exit 1
fi

if grep -Eq 'RT_WAITING_FOREVER' \
    "$root/big_core/mpp_pipeline/mpp_pipeline.c" \
    "$root/big_core/mpp_pipeline/motion_thread.c"; then
    echo "shutdown waits must be bounded, not RT_WAITING_FOREVER"
    exit 1
fi

enabled_main=$(awk '
    /^[[:space:]]*#if[[:space:]]+0([[:space:]]|$)/ { disabled = 1; next }
    disabled && /^[[:space:]]*#else([[:space:]]|$)/ { disabled = 0; in_if0_else = 1; next }
    disabled && /^[[:space:]]*#endif([[:space:]]|$)/ { disabled = 0; next }
    in_if0_else && /^[[:space:]]*#endif([[:space:]]|$)/ { in_if0_else = 0; next }
    !disabled { print }
' "$root/big_core/mpp_pipeline/mpp_pipeline.c")
if printf '%s\n' "$enabled_main" | grep -Eq 'AUTO_EXIT_SEC|Auto-exit timeout reached|difftime\(time\(NULL\), start\)'; then
    echo "main loop must not enable AUTO_EXIT_SEC auto-exit"
    exit 1
fi
if ! printf '%s\n' "$enabled_main" | grep -Eq 'while[[:space:]]*\([[:space:]]*g_running[[:space:]]*\)'; then
    echo "main loop must keep running until g_running is cleared"
    exit 1
fi

stream_thread_body=$(sed -n '/void stream_thread/,/^}/p' "$root/big_core/mpp_pipeline/media_venc.c")
if ! printf '%s\n' "$stream_thread_body" | grep -q 'g_stream_running'; then
    echo "stream_thread must use g_stream_running as its local run flag"
    exit 1
fi
if printf '%s\n' "$stream_thread_body" | grep -q 'while (g_running)'; then
    echo "stream_thread must not exit directly on g_running"
    exit 1
fi

ai_motion_file="$root/big_core/mpp_pipeline/motion_thread.c"
ai_release_line=$(grep -n 'ai_frame_release(ai_frame_handle)' "$ai_motion_file" | head -n1 | cut -d: -f1)
ai_osd_line=$(grep -n 'osd_set_motion_visible(1' "$ai_motion_file" | head -n1 | cut -d: -f1)
if [ -z "$ai_release_line" ] || [ -z "$ai_osd_line" ]; then
    echo "ai motion thread must contain ai_frame_release and osd_set_motion_visible"
    exit 1
fi

if [ "$ai_release_line" -ge "$ai_osd_line" ]; then
    echo "ai_frame_release must run before osd_set_motion_visible"
    exit 1
fi

if ! grep -q 'osd_poll_auto_hide();' "$ai_motion_file"; then
    echo "ai motion thread must poll OSD auto hide"
    exit 1
fi

osd_file="$root/big_core/mpp_pipeline/media_osd.c"
if grep -Eq 'rt_timer_(create|control|start|stop|delete)' "$osd_file"; then
    echo "OSD auto hide must not use RT timer APIs"
    exit 1
fi

if grep -q 'rt_thread_create("osdhide"' "$osd_file"; then
    echo "OSD auto hide must not create a background hide thread"
    exit 1
fi

if grep -Eq 'g_osd_lock|osd_lock_take|osd_lock_release|OSD_LOCK_TIMEOUT_MS' "$osd_file"; then
    echo "OSD auto hide must not keep the debug-era OSD lock"
    exit 1
fi

if grep -q 'osd_control_stop' \
    "$osd_file" \
    "$root/big_core/mpp_pipeline/mpp_pipeline.h" \
    "$root/big_core/mpp_pipeline/media_cleanup.c"; then
    echo "OSD control stop must be folded into osd_deinit"
    exit 1
fi

if grep -Eq 'OSD motion request (start|done)|OSD set motion visible (start|done)|kd_mpi_venc_set_2d_osd_param (start|done)' \
    "$ai_motion_file" \
    "$osd_file"; then
    echo "normal OSD path must not keep debug start/done logs"
    exit 1
fi

cleanup_file="$root/big_core/mpp_pipeline/media_cleanup.c"
ai_motion_stop_line=$(grep -n 'ai_motion_thread_stop();' "$cleanup_file" | head -n1 | cut -d: -f1)
osd_deinit_line=$(grep -n 'osd_deinit();' "$cleanup_file" | head -n1 | cut -d: -f1)
stream_export_deinit_line=$(grep -n 'stream_export_deinit();' "$cleanup_file" | head -n1 | cut -d: -f1)
vicap_stop_line=$(grep -n 'kd_mpi_vicap_stop_stream' "$cleanup_file" | head -n1 | cut -d: -f1)
stream_stop_line=$(grep -n 'stream_thread_stop' "$cleanup_file" | head -n1 | cut -d: -f1)
venc_stop_line=$(grep -n 'kd_mpi_venc_stop_chn' "$cleanup_file" | head -n1 | cut -d: -f1)
venc_destroy_line=$(grep -n 'kd_mpi_venc_destroy_chn' "$cleanup_file" | head -n1 | cut -d: -f1)

if [ -z "$ai_motion_stop_line" ] || [ -z "$osd_deinit_line" ] || \
   [ -z "$stream_export_deinit_line" ] || \
   [ -z "$vicap_stop_line" ] || [ -z "$stream_stop_line" ] || \
   [ -z "$venc_stop_line" ] || [ -z "$venc_destroy_line" ]; then
    echo "cleanup must contain ai_motion_thread_stop, vicap stop, stream_thread_stop, stream_export_deinit, osd_deinit, venc stop, and venc destroy"
    exit 1
fi

if [ "$stream_stop_line" -le "$vicap_stop_line" ]; then
    echo "stream_thread_stop must run after kd_mpi_vicap_stop_stream"
    exit 1
fi

if [ "$stream_export_deinit_line" -le "$stream_stop_line" ]; then
    echo "stream_export_deinit must run after stream_thread_stop"
    exit 1
fi

if [ "$osd_deinit_line" -le "$venc_stop_line" ]; then
    echo "osd_deinit must run after kd_mpi_venc_stop_chn"
    exit 1
fi

if [ "$osd_deinit_line" -ge "$venc_destroy_line" ]; then
    echo "osd_deinit must run before kd_mpi_venc_destroy_chn"
    exit 1
fi

if grep -q 'rt_timer_delete(g_osd_hide_timer)' "$osd_file"; then
    echo "osd control cleanup must not call rt_timer_delete on shutdown path"
    exit 1
fi

echo "shutdown source rules passed"
