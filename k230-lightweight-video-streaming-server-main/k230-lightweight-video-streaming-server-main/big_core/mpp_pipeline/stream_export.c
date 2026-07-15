#include "mpp_pipeline.h"

#include "mpp_nalu_ipc.h"

/* DATAFIFO backend is private to this export layer. */
k_s32 nalu_ipc_init(void);
k_s32 nalu_ipc_submit_stream(k_u32 chn,
                             const k_venc_stream *stream,
                             k_u32 flags);
k_s32 nalu_ipc_flush(void);
k_u32 nalu_ipc_get_pending_count(void);
void nalu_ipc_deinit(void);

#define LOCAL_LOG_FRAME_INTERVAL  30U
#define SNAPSHOT_PENDING_MAX      4U
#define SNAPSHOT_WAIT_IDR_FRAMES  (VENC_GOP + 2U)

static stream_export_mode g_stream_export_mode = STREAM_EXPORT_LOCAL_LOG;
static k_u32 g_stream_export_frame_count = 0;
static k_bool g_stream_export_inited = K_FALSE;
static k_u32 g_pending_snapshot_count;
static k_u32 g_snapshot_wait_frames;
static k_u32 g_snapshot_request_head;
static k_u32 g_snapshot_request_tail;
static snapshot_request_msg g_snapshot_requests[SNAPSHOT_PENDING_MAX];

static void stream_export_reset_snapshot_requests(void)
{
    rt_enter_critical();
    g_pending_snapshot_count = 0;
    g_snapshot_wait_frames = 0;
    g_snapshot_request_head = 0;
    g_snapshot_request_tail = 0;
    memset(g_snapshot_requests, 0, sizeof(g_snapshot_requests));
    rt_exit_critical();
}

static const char *stream_export_mode_name(stream_export_mode mode)
{
    if (mode == STREAM_EXPORT_DATAFIFO)
        return "datafifo";

    return "local-log";
}

k_s32 stream_export_request_snapshot(const snapshot_request_msg *request)
{
    snapshot_request_msg req;
    k_u32 pending;

    memset(&req, 0, sizeof(req));
    if (request)
        req = *request;

    rt_enter_critical();
    if (g_pending_snapshot_count >= SNAPSHOT_PENDING_MAX) {
        pending = g_pending_snapshot_count;
        rt_exit_critical();
        LOG("Snapshot request queue full, drop event_id=%u pending=%u",
            req.event_id, pending);
        return -1;
    }

    g_snapshot_requests[g_snapshot_request_tail] = req;
    g_snapshot_request_tail = (g_snapshot_request_tail + 1U) % SNAPSHOT_PENDING_MAX;
    g_pending_snapshot_count++;
    g_snapshot_wait_frames = 0;
    pending = g_pending_snapshot_count;
    rt_exit_critical();

    LOG("Snapshot request queued: event_id=%u source_chn=%u pending=%u hint=%s",
        req.event_id,
        req.source_chn,
        pending,
        req.path_hint[0] ? req.path_hint : "motion");
    return 0;
}

static k_bool stream_export_has_header_pack(const k_venc_stream *stream)
{
    if (!stream || !stream->pack)
        return K_FALSE;

    for (k_u32 i = 0; i < stream->pack_cnt; i++) {
        if (stream->pack[i].type == K_VENC_HEADER)
            return K_TRUE;
    }

    return K_FALSE;
}

static k_u32 stream_export_select_snapshot_flags(const k_venc_stream *stream)
{
    k_u32 flags = 0;
    k_bool has_header = stream_export_has_header_pack(stream);

    rt_enter_critical();
    if (g_pending_snapshot_count > 0) {
        if (has_header || g_snapshot_wait_frames >= SNAPSHOT_WAIT_IDR_FRAMES) {
            flags |= MPP_NALU_IPC_FLAG_SNAPSHOT;
        } else {
            g_snapshot_wait_frames++;
        }
    }
    rt_exit_critical();

    return flags;
}

static void stream_export_consume_snapshot_request(snapshot_request_msg *request)
{
    rt_enter_critical();
    if (g_pending_snapshot_count > 0) {
        if (request)
            *request = g_snapshot_requests[g_snapshot_request_head];

        memset(&g_snapshot_requests[g_snapshot_request_head],
               0,
               sizeof(g_snapshot_requests[g_snapshot_request_head]));
        g_snapshot_request_head = (g_snapshot_request_head + 1U) % SNAPSHOT_PENDING_MAX;
        g_pending_snapshot_count--;
        g_snapshot_wait_frames = 0;
        if (g_pending_snapshot_count == 0) {
            g_snapshot_request_head = 0;
            g_snapshot_request_tail = 0;
        }
    } else if (request) {
        memset(request, 0, sizeof(*request));
    }
    rt_exit_critical();
}

k_s32 stream_export_init(stream_export_mode mode)
{
    k_s32 ret = 0;

    stream_export_reset_snapshot_requests();
    g_stream_export_mode = mode;
    g_stream_export_frame_count = 0;
    g_stream_export_inited = K_TRUE;

    if (mode == STREAM_EXPORT_DATAFIFO) {
        ret = nalu_ipc_init();
        if (ret) {
            g_stream_export_inited = K_FALSE;
            return ret;
        }
    }

    LOG("Stream export mode: %s", stream_export_mode_name(mode));
    return 0;
}

static k_s32 stream_export_submit_local_log(const k_venc_stream *stream)
{
    for (k_u32 i = 0; i < stream->pack_cnt; i++) {
        if (stream->pack[i].type != K_VENC_HEADER) {
            g_stream_export_frame_count++;
            if (g_stream_export_frame_count == 1 ||
                (g_stream_export_frame_count % LOCAL_LOG_FRAME_INTERVAL) == 0) {
                LOG("Get NALU, Size: %u bytes  [frame #%u]",
                    stream->pack[i].len, g_stream_export_frame_count);
            }
        } else {
            LOG("Get NALU (SPS/PPS header), Size: %u bytes", stream->pack[i].len);
        }
    }

    return 0;
}

k_s32 stream_export_submit_venc_stream(k_u32 chn,
                                       const k_venc_stream *stream,
                                       k_bool *release_by_caller)
{
    k_s32 ret;
    k_u32 snapshot_flags;

    if (release_by_caller)
        *release_by_caller = K_TRUE;

    if (!release_by_caller || !stream || !stream->pack || stream->pack_cnt == 0)
        return -1;

    if (!g_stream_export_inited)
        return -1;

    snapshot_flags = stream_export_select_snapshot_flags(stream);

    if (g_stream_export_mode == STREAM_EXPORT_DATAFIFO) {
        ret = nalu_ipc_submit_stream(chn, stream, snapshot_flags);
        if (ret) {
            *release_by_caller = K_TRUE;
            return ret;
        }

        if (snapshot_flags & MPP_NALU_IPC_FLAG_SNAPSHOT) {
            snapshot_request_msg request;

            stream_export_consume_snapshot_request(&request);
            LOG("Snapshot request delivered to DATAFIFO: event_id=%u reserved=0x%x",
                request.event_id,
                snapshot_flags);
        }

        *release_by_caller = K_FALSE;
        return 0;
    }

    *release_by_caller = K_TRUE;
    if (g_stream_export_mode == STREAM_EXPORT_LOCAL_LOG) {
        ret = stream_export_submit_local_log(stream);
        if (!ret && (snapshot_flags & MPP_NALU_IPC_FLAG_SNAPSHOT)) {
            snapshot_request_msg request;

            stream_export_consume_snapshot_request(&request);
            LOG("Snapshot mock IPC event: event_id=%u reserved=0x%x hint=%s",
                request.event_id,
                snapshot_flags,
                request.path_hint[0] ? request.path_hint : "motion");
        }

        return ret;
    }

    return 0;
}

k_s32 stream_export_flush(void)
{
    if (!g_stream_export_inited)
        return 0;

    if (g_stream_export_mode == STREAM_EXPORT_DATAFIFO)
        return nalu_ipc_flush();

    return 0;
}

k_u32 stream_export_get_pending_count(void)
{
    if (!g_stream_export_inited)
        return 0;

    if (g_stream_export_mode == STREAM_EXPORT_DATAFIFO)
        return nalu_ipc_get_pending_count();

    return 0;
}

void stream_export_deinit(void)
{
    if (!g_stream_export_inited)
        return;

    if (g_stream_export_mode == STREAM_EXPORT_DATAFIFO)
        nalu_ipc_deinit();

    stream_export_reset_snapshot_requests();

    LOG("Stream export deinit");
    g_stream_export_inited = K_FALSE;
}
