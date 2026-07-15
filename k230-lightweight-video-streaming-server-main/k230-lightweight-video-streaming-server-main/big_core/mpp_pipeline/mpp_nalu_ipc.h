#ifndef MPP_NALU_IPC_H
#define MPP_NALU_IPC_H

#include "k_type.h"

#define MPP_NALU_IPC_MAGIC      0x4E414C55U  /* "NALU" */
#define MPP_NALU_IPC_VERSION    1U
#define MPP_NALU_IPC_MAX_PACKS  8U
#define MPP_NALU_IPC_ITEM_SIZE  512U

#define MPP_NALU_IPC_FLAG_SNAPSHOT  (1U << 0)

typedef struct {
    k_u64 phys_addr;
    k_u64 pts;
    k_u32 len;
    k_u32 type;
} mpp_nalu_ipc_pack;

typedef struct {
    k_u32 magic;
    k_u32 version;
    k_u32 chn;
    k_u32 pack_cnt;
    k_u64 seq;
    k_u64 frame_pts;
    k_u64 submit_time_ms;
    k_u32 total_len;
    k_u32 reserved;
    mpp_nalu_ipc_pack packs[MPP_NALU_IPC_MAX_PACKS];
} mpp_nalu_ipc_msg;

typedef char mpp_nalu_ipc_msg_must_fit_item[
    (sizeof(mpp_nalu_ipc_msg) <= MPP_NALU_IPC_ITEM_SIZE) ? 1 : -1
];

#endif /* MPP_NALU_IPC_H */
