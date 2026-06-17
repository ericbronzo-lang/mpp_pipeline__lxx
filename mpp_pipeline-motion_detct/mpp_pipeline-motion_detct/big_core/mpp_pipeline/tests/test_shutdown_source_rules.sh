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

stream_thread_body=$(sed -n '/void stream_thread/,/^}/p' "$root/big_core/mpp_pipeline/media_venc.c")
if ! printf '%s\n' "$stream_thread_body" | grep -q 'g_stream_running'; then
    echo "stream_thread must use g_stream_running as its local run flag"
    exit 1
fi
if printf '%s\n' "$stream_thread_body" | grep -q 'while (g_running)'; then
    echo "stream_thread must not exit directly on g_running"
    exit 1
fi

cleanup_file="$root/big_core/mpp_pipeline/media_cleanup.c"
ai_motion_stop_line=$(grep -n 'ai_motion_thread_stop();' "$cleanup_file" | head -n1 | cut -d: -f1)
osd_control_stop_line=$(grep -n 'osd_control_stop();' "$cleanup_file" | head -n1 | cut -d: -f1)
osd_deinit_line=$(grep -n 'osd_deinit();' "$cleanup_file" | head -n1 | cut -d: -f1)
stream_export_deinit_line=$(grep -n 'stream_export_deinit();' "$cleanup_file" | head -n1 | cut -d: -f1)
vicap_stop_line=$(grep -n 'kd_mpi_vicap_stop_stream' "$cleanup_file" | head -n1 | cut -d: -f1)
stream_stop_line=$(grep -n 'stream_thread_stop' "$cleanup_file" | head -n1 | cut -d: -f1)
venc_stop_line=$(grep -n 'kd_mpi_venc_stop_chn' "$cleanup_file" | head -n1 | cut -d: -f1)
venc_destroy_line=$(grep -n 'kd_mpi_venc_destroy_chn' "$cleanup_file" | head -n1 | cut -d: -f1)

if [ -z "$ai_motion_stop_line" ] || [ -z "$osd_control_stop_line" ] || \
   [ -z "$osd_deinit_line" ] || \
   [ -z "$stream_export_deinit_line" ] || \
   [ -z "$vicap_stop_line" ] || [ -z "$stream_stop_line" ] || \
   [ -z "$venc_stop_line" ] || [ -z "$venc_destroy_line" ]; then
    echo "cleanup must contain ai_motion_thread_stop, osd_control_stop, vicap stop, stream_thread_stop, stream_export_deinit, osd_deinit, venc stop, and venc destroy"
    exit 1
fi

if [ "$osd_control_stop_line" -le "$ai_motion_stop_line" ]; then
    echo "osd_control_stop must run after ai_motion_thread_stop"
    exit 1
fi

if [ "$stream_stop_line" -le "$vicap_stop_line" ]; then
    echo "stream_thread_stop must run after kd_mpi_vicap_stop_stream"
    exit 1
fi

if [ "$osd_control_stop_line" -le "$stream_stop_line" ]; then
    echo "osd_control_stop must run after stream_thread_stop"
    exit 1
fi

if [ "$osd_control_stop_line" -ge "$venc_stop_line" ]; then
    echo "osd_control_stop must run before kd_mpi_venc_stop_chn"
    exit 1
fi

if [ "$osd_deinit_line" -le "$osd_control_stop_line" ]; then
    echo "osd_deinit must run after osd_control_stop"
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

if grep -q 'rt_timer_delete(g_osd_hide_timer)' "$root/big_core/mpp_pipeline/media_osd.c"; then
    echo "osd control cleanup must not call rt_timer_delete on shutdown path"
    exit 1
fi

echo "shutdown source rules passed"
