#include "mpp_pipeline.h"

/* DATAFIFO backend is private to this export layer. */
k_s32 nalu_ipc_init(void);
k_s32 nalu_ipc_submit_stream(k_u32 chn, const k_venc_stream *stream);
k_s32 nalu_ipc_flush(void);
void nalu_ipc_deinit(void);

static stream_export_mode g_stream_export_mode = STREAM_EXPORT_LOCAL_LOG;
static k_u32 g_stream_export_frame_count = 0;
static k_bool g_stream_export_inited = K_FALSE;

static const char *stream_export_mode_name(stream_export_mode mode)
{
    if (mode == STREAM_EXPORT_DATAFIFO)
        return "datafifo";

    return "local-log";
}

k_s32 stream_export_init(stream_export_mode mode)
{
    k_s32 ret = 0;

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
            LOG("Get NALU, Size: %u bytes  [frame #%u]",
                stream->pack[i].len, g_stream_export_frame_count);
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

    if (release_by_caller)
        *release_by_caller = K_TRUE;

    if (!release_by_caller || !stream || !stream->pack || stream->pack_cnt == 0)
        return -1;

    if (!g_stream_export_inited)
        return -1;

    if (g_stream_export_mode == STREAM_EXPORT_DATAFIFO) {
        ret = nalu_ipc_submit_stream(chn, stream);
        if (ret) {
            *release_by_caller = K_TRUE;
            return ret;
        }

        *release_by_caller = K_FALSE;
        return 0;
    }

    *release_by_caller = K_TRUE;
    if (g_stream_export_mode == STREAM_EXPORT_LOCAL_LOG)
        return stream_export_submit_local_log(stream);

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

void stream_export_deinit(void)
{
    if (!g_stream_export_inited)
        return;

    if (g_stream_export_mode == STREAM_EXPORT_DATAFIFO)
        nalu_ipc_deinit();

    LOG("Stream export deinit");
    g_stream_export_inited = K_FALSE;
}
