/**
  - pack = 单个码流块的描述
  - stream = 一帧/一组 pack 的 SDK 容器
  - msg = 跨核传输用的自定义容器
  - submit = 把当前 stream 转成 msg，交给 DATAFIFO
   */
#include "mpp_pipeline.h"

#include "k_datafifo.h"
#include "mpp_nalu_ipc.h"

#define NALU_IPC_PENDING_MAX  OUTPUT_BUF_CNT
#define NALU_IPC_FIFO_ENTRIES NALU_IPC_PENDING_MAX

typedef struct {
    k_bool in_use;
    /* ipc_msg is both the DATAFIFO item and the delayed-release credential. */
    mpp_nalu_ipc_msg ipc_msg;
} nalu_ipc_pending_item;

static k_datafifo_handle g_nalu_fifo = K_DATAFIFO_INVALID_HANDLE;
static k_bool g_nalu_fifo_inited = K_FALSE;
static k_u64 g_nalu_fifo_phy_addr;
static k_u64 g_next_nalu_msg_seq;
/* Tracks streams that were handed to DATAFIFO but not yet READ_DONE by Linux. */
static nalu_ipc_pending_item g_pending[NALU_IPC_PENDING_MAX];

static k_datafifo_params_s g_nalu_fifo_params = {
    NALU_IPC_FIFO_ENTRIES,  /**< The number of items in the ring buffer*/
    MPP_NALU_IPC_ITEM_SIZE, /**< Item size*/
    K_TRUE,                 /**<Whether the data buffer release by writer*/
    DATAFIFO_WRITER         /**<READER or WRITER*/
};
static void nalu_ipc_msg_to_release_stream(const mpp_nalu_ipc_msg *msg,
                                           k_venc_pack *packs,
                                           k_venc_stream *stream)
{
    memset(packs, 0, sizeof(k_venc_pack) * MPP_NALU_IPC_MAX_PACKS);
    memset(stream, 0, sizeof(*stream));

    stream->pack_cnt = msg->pack_cnt;
    stream->pack = packs;

    for (k_u32 i = 0; i < msg->pack_cnt && i < MPP_NALU_IPC_MAX_PACKS; i++) {
        packs[i].phys_addr = msg->packs[i].phys_addr;
        packs[i].len = msg->packs[i].len;
        packs[i].pts = msg->packs[i].pts;
        packs[i].type = (k_venc_pack_type)msg->packs[i].type;
    }
}

static void nalu_ipc_release_msg_stream(const mpp_nalu_ipc_msg *msg)
{
    k_s32 ret;
    k_venc_stream stream;
    k_venc_pack packs[MPP_NALU_IPC_MAX_PACKS];

    if (!msg || msg->magic != MPP_NALU_IPC_MAGIC || msg->pack_cnt == 0)
        return;

    nalu_ipc_msg_to_release_stream(msg, packs, &stream);
    ret = kd_mpi_venc_release_stream(msg->chn, &stream); //释放内存buffer
    CHECK_RET(ret, __func__, __LINE__);
}

//小核读完后，找回这个“释放凭据”
static nalu_ipc_pending_item *nalu_ipc_find_pending(const mpp_nalu_ipc_msg *msg)
{
    if (!msg)
        return NULL;

    for (k_u32 i = 0; i < NALU_IPC_PENDING_MAX; i++) {
        if (g_pending[i].in_use &&
            g_pending[i].ipc_msg.seq == msg->seq &&
            g_pending[i].ipc_msg.chn == msg->chn)
            return &g_pending[i];
    }

    return NULL;
}

//发给小核前，找地方存“释放凭据”
static nalu_ipc_pending_item *nalu_ipc_alloc_pending(void) // 寻找可用pending item
{
    for (k_u32 i = 0; i < NALU_IPC_PENDING_MAX; i++) {
        if (!g_pending[i].in_use)
            return &g_pending[i];
    }

    return NULL;
}

static void nalu_ipc_release_callback(void *datafifo_item)
{
    mpp_nalu_ipc_msg *msg = (mpp_nalu_ipc_msg *)datafifo_item;
    nalu_ipc_pending_item *pending = nalu_ipc_find_pending(msg);

    if (!pending) {
        LOG("NALU IPC release callback: unknown stream msg=%p", datafifo_item);
        return;
    }

    nalu_ipc_release_msg_stream(&pending->ipc_msg);
    memset(pending, 0, sizeof(*pending));
}

k_s32 nalu_ipc_init(void)
{
    k_s32 ret;

    if (g_nalu_fifo_inited)
        return 0;

    memset(g_pending, 0, sizeof(g_pending));
    g_next_nalu_msg_seq = 0;
    g_nalu_fifo_phy_addr = 0;

    ret = kd_datafifo_open(&g_nalu_fifo, &g_nalu_fifo_params);
    if (ret) {
        LOG("kd_datafifo_open failed! ret=0x%x", ret);
        g_nalu_fifo = K_DATAFIFO_INVALID_HANDLE;
        return ret;
    }

    /**<Get the physic address of ring buffer*/
    ret = kd_datafifo_cmd(g_nalu_fifo, DATAFIFO_CMD_GET_PHY_ADDR, &g_nalu_fifo_phy_addr);
    if (ret) {
        LOG("DATAFIFO_CMD_GET_PHY_ADDR failed! ret=0x%x", ret);
        kd_datafifo_close(g_nalu_fifo);
        g_nalu_fifo = K_DATAFIFO_INVALID_HANDLE;
        return ret;
    }

    /*<When bDataReleaseByWriter is K_TRUE, 
    the writer should call this to register release callback*/
    ret = kd_datafifo_cmd(g_nalu_fifo,
                          DATAFIFO_CMD_SET_DATA_RELEASE_CALLBACK,
                          nalu_ipc_release_callback);
    if (ret) {
        LOG("DATAFIFO_CMD_SET_DATA_RELEASE_CALLBACK failed! ret=0x%x", ret);
        kd_datafifo_close(g_nalu_fifo);
        g_nalu_fifo = K_DATAFIFO_INVALID_HANDLE;
        return ret;
    }

    g_nalu_fifo_inited = K_TRUE;
    LOG("NALU IPC DATAFIFO init OK: phy_addr=0x%llx",
        (unsigned long long)g_nalu_fifo_phy_addr);

    return 0;
}

static void nalu_ipc_build_msg(mpp_nalu_ipc_msg *msg,
                               k_u32 chn,
                               const k_venc_stream *stream)
{
    memset(msg, 0, sizeof(*msg));

    msg->magic = MPP_NALU_IPC_MAGIC;
    msg->version = MPP_NALU_IPC_VERSION;
    msg->chn = chn;
    msg->pack_cnt = stream->pack_cnt;
    msg->seq = ++g_next_nalu_msg_seq;
    if (stream->pack_cnt > 0)
        msg->frame_pts = stream->pack[0].pts;

    for (k_u32 i = 0; i < stream->pack_cnt && i < MPP_NALU_IPC_MAX_PACKS; i++) {
        msg->packs[i].phys_addr = stream->pack[i].phys_addr;
        msg->packs[i].pts = stream->pack[i].pts;
        msg->packs[i].len = stream->pack[i].len;
        msg->packs[i].type = stream->pack[i].type;
        msg->total_len += stream->pack[i].len;
    }
}

k_s32 nalu_ipc_submit_stream(k_u32 chn, const k_venc_stream *stream)
{
    k_s32 ret;
    k_u32 avail_write_len = 0;
    nalu_ipc_pending_item *pending;

    if (!g_nalu_fifo_inited || g_nalu_fifo == K_DATAFIFO_INVALID_HANDLE)
        return -1;
    if (!stream || !stream->pack || stream->pack_cnt == 0)
        return -1;
    if (stream->pack_cnt > MPP_NALU_IPC_MAX_PACKS) {
        LOG("NALU IPC pack_cnt=%u exceeds max=%u",
            stream->pack_cnt, MPP_NALU_IPC_MAX_PACKS);
        return -1;
    }

    /* A successful submit transfers release responsibility to this backend. */
    /* Flush release notifications from the reader side before checking space. */
    ret = kd_datafifo_write(g_nalu_fifo, NULL);
    if (ret) {
        LOG("kd_datafifo_write(NULL) failed! ret=0x%x", ret);
        return ret;
    }

    /**<Get available write length*/
    ret = kd_datafifo_cmd(g_nalu_fifo,
                          DATAFIFO_CMD_GET_AVAIL_WRITE_LEN,
                          &avail_write_len);
    if (ret) {
        LOG("DATAFIFO_CMD_GET_AVAIL_WRITE_LEN failed! ret=0x%x", ret);
        return ret;
    }
    if (avail_write_len < MPP_NALU_IPC_ITEM_SIZE) {
        LOG("NALU IPC DATAFIFO full: avail=%u, drop current stream", avail_write_len);
        return -1;
    }

    pending = nalu_ipc_alloc_pending();
    if (!pending) {
        LOG("NALU IPC pending table full, drop current stream");
        return -1;
    }

    nalu_ipc_build_msg(&pending->ipc_msg, chn, stream);
    pending->in_use = K_TRUE;

    ret = kd_datafifo_write(g_nalu_fifo, &pending->ipc_msg);
    if (ret) {
        LOG("kd_datafifo_write failed! ret=0x%x", ret);
        memset(pending, 0, sizeof(*pending));
        return ret;
    }

    /**<When the writer buffer is write done, the writer should call this function*/
    ret = kd_datafifo_cmd(g_nalu_fifo, DATAFIFO_CMD_WRITE_DONE, NULL);
    if (ret) {
        LOG("DATAFIFO_CMD_WRITE_DONE failed! ret=0x%x", ret);
        memset(pending, 0, sizeof(*pending));
        return ret;
    }

    return 0;
}

k_s32 nalu_ipc_flush(void)
{
    if (!g_nalu_fifo_inited || g_nalu_fifo == K_DATAFIFO_INVALID_HANDLE)
        return 0;

    return kd_datafifo_write(g_nalu_fifo, NULL);
}

k_u64 nalu_ipc_get_phy_addr(void)
{
    return g_nalu_fifo_phy_addr;
}

void nalu_ipc_deinit(void)
{
    if (!g_nalu_fifo_inited || g_nalu_fifo == K_DATAFIFO_INVALID_HANDLE)
        return;

    kd_datafifo_write(g_nalu_fifo, NULL);

    for (k_u32 i = 0; i < NALU_IPC_PENDING_MAX; i++) {
        if (g_pending[i].in_use) {
            LOG("NALU IPC force release pending seq=%llu",
                (unsigned long long)g_pending[i].ipc_msg.seq);
            nalu_ipc_release_msg_stream(&g_pending[i].ipc_msg);
            memset(&g_pending[i], 0, sizeof(g_pending[i]));
        }
    }

    kd_datafifo_close(g_nalu_fifo);
    g_nalu_fifo = K_DATAFIFO_INVALID_HANDLE;
    g_nalu_fifo_phy_addr = 0;
    g_nalu_fifo_inited = K_FALSE;
    LOG("NALU IPC DATAFIFO deinit");
}
