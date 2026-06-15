#include "mpp_pipeline.h"

static k_bool g_osd_stub_inited = K_FALSE;

k_s32 osd_init(void)
{
    g_osd_stub_inited = K_TRUE;
    LOG("OSD init stub: waiting for VENC 2D OSD MMZ buffer wiring");
    return 0;
}

k_s32 osd_set_motion_visible(k_u32 visible, k_u32 duration_ms)
{
    if (!g_osd_stub_inited)
        LOG("OSD motion stub called before osd_init");

    LOG("OSD motion visible=%u duration=%ums (stub)", visible, duration_ms);
    return 0;
}

void osd_deinit(void)
{
    if (!g_osd_stub_inited)
        return;

    LOG("OSD deinit stub");
    g_osd_stub_inited = K_FALSE;
}
