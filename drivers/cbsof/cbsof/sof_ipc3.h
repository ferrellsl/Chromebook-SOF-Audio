// Sound Open Firmware IPC3 ABI subset (SOF stable-v2.2).
// Rewritten from thesofproject/sof src/include/ipc/*.h and
// src/include/kernel/ext_manifest.h, BSD-3-Clause.
// Copyright(c) 2018 Intel Corporation. All rights reserved.
#pragma once

#define SOF_GLB_TYPE_SHIFT          28
#define SOF_GLB_TYPE_MASK           (0xFu << SOF_GLB_TYPE_SHIFT)
#define SOF_GLB_TYPE(x)             ((ULONG)(x) << SOF_GLB_TYPE_SHIFT)
#define SOF_CMD_TYPE_SHIFT          16
#define SOF_CMD_TYPE_MASK           (0xFFFu << SOF_CMD_TYPE_SHIFT)
#define SOF_CMD_TYPE(x)             ((ULONG)(x) << SOF_CMD_TYPE_SHIFT)

#define SOF_IPC_GLB_REPLY           SOF_GLB_TYPE(0x1)
#define SOF_IPC_GLB_COMPOUND        SOF_GLB_TYPE(0x2)
#define SOF_IPC_GLB_TPLG_MSG        SOF_GLB_TYPE(0x3)
#define SOF_IPC_GLB_PM_MSG          SOF_GLB_TYPE(0x4)
#define SOF_IPC_GLB_COMP_MSG        SOF_GLB_TYPE(0x5)
#define SOF_IPC_GLB_STREAM_MSG      SOF_GLB_TYPE(0x6)
#define SOF_IPC_FW_READY            SOF_GLB_TYPE(0x7)
#define SOF_IPC_GLB_DAI_MSG         SOF_GLB_TYPE(0x8)
#define SOF_IPC_GLB_TRACE_MSG       SOF_GLB_TYPE(0x9)

#define SOF_IPC_PM_CTX_SAVE         SOF_CMD_TYPE(0x001)
#define SOF_IPC_PM_CTX_RESTORE      SOF_CMD_TYPE(0x002)
#define SOF_IPC_PM_CTX_SIZE         SOF_CMD_TYPE(0x003)

#define SOF_IPC_PANIC_MAGIC         0x0dead000
#define SOF_IPC_PANIC_MAGIC_MASK    0x0ffff000

#define SOF_IPC_MSG_MAX_SIZE        384
#define SOF_IPC_MAX_ELEMS           16

#define SOF_EXT_MAN_MAGIC_NUMBER    0x6e614d58  // "XMan"

enum sof_ipc_ext_type {
    SOF_IPC_EXT_UNUSED = 0,
    SOF_IPC_EXT_WINDOW = 1,
    SOF_IPC_EXT_CC_INFO = 2,
    SOF_IPC_EXT_PROBE_INFO = 3,
    SOF_IPC_EXT_USER_ABI_INFO = 4,
};

enum sof_ipc_region {
    SOF_IPC_REGION_DOWNBOX = 0,
    SOF_IPC_REGION_UPBOX,
    SOF_IPC_REGION_TRACE,
    SOF_IPC_REGION_DEBUG,
    SOF_IPC_REGION_STREAM,
    SOF_IPC_REGION_REGS,
    SOF_IPC_REGION_EXCEPTION,
};

#pragma pack(push, 4)

struct sof_ext_man_header {
    UINT32 magic;
    UINT32 full_size;
    UINT32 header_size;
    UINT32 header_version;
};

#define SOF_EXT_MAN_ELEM_FW_VERSION 0
#define SOF_EXT_MAN_ELEM_WINDOW     1

struct sof_ext_man_elem_header {
    UINT32 type;
    UINT32 size;    // including this header
};

struct sof_ipc_hdr {
    UINT32 size;
};

struct sof_ipc_cmd_hdr {
    UINT32 size;
    UINT32 cmd;
};

struct sof_ipc_reply {
    struct sof_ipc_cmd_hdr hdr;
    INT32 error;
};

struct sof_ipc_fw_version {
    struct sof_ipc_hdr hdr;
    UINT16 major;
    UINT16 minor;
    UINT16 micro;
    UINT16 build;
    UINT8 date[12];
    UINT8 time[10];
    UINT8 tag[6];
    UINT32 abi_version;
    UINT32 src_hash;
    UINT32 reserved[3];
};

struct sof_ipc_fw_ready {
    struct sof_ipc_cmd_hdr hdr;
    UINT32 dspbox_offset;
    UINT32 hostbox_offset;
    UINT32 dspbox_size;
    UINT32 hostbox_size;
    struct sof_ipc_fw_version version;
    UINT64 flags;
    UINT32 reserved[4];
};

struct sof_ipc_ext_data_hdr {
    struct sof_ipc_cmd_hdr hdr;
    UINT32 type;
};

struct sof_ipc_window_elem {
    struct sof_ipc_hdr hdr;
    UINT32 type;
    UINT32 id;
    UINT32 flags;
    UINT32 size;
    UINT32 offset;
};

struct sof_ipc_window {
    struct sof_ipc_ext_data_hdr ext_hdr;
    UINT32 num_windows;
    struct sof_ipc_window_elem window[SOF_IPC_MAX_ELEMS];
};

struct sof_ipc_host_buffer {
    struct sof_ipc_hdr hdr;
    UINT32 phy_addr;
    UINT32 pages;
    UINT32 size;
    UINT32 reserved[3];
};

struct sof_ipc_pm_ctx {
    struct sof_ipc_cmd_hdr hdr;
    struct sof_ipc_host_buffer buffer;
    UINT32 num_elems;
    UINT32 size;
    UINT32 reserved[8];
};

struct sof_ipc_stream_params {
    struct sof_ipc_hdr hdr;
    struct sof_ipc_host_buffer buffer;
    UINT32 direction;
    UINT32 frame_fmt;
    UINT32 buffer_fmt;
    UINT32 rate;
    UINT16 stream_tag;
    UINT16 channels;
    UINT16 sample_valid_bytes;
    UINT16 sample_container_bytes;
    UINT32 host_period_bytes;
    UINT16 no_stream_position;
    UINT8 cont_update_posn;
    UINT8 reserved0;
    INT16 ext_data_length;
    UINT8 reserved[2];
    UINT16 chmap[8];
};

struct sof_ipc_pcm_params {
    struct sof_ipc_cmd_hdr hdr;
    UINT32 comp_id;
    UINT32 flags;
    UINT32 reserved[2];
    struct sof_ipc_stream_params params;
};

struct sof_ipc_pcm_params_reply {
    struct sof_ipc_reply rhdr;
    UINT32 comp_id;
    UINT32 posn_offset;
};

struct sof_ipc_stream {
    struct sof_ipc_cmd_hdr hdr;
    UINT32 comp_id;
};

#pragma pack(pop)

#define SOF_IPC_STREAM_PCM_PARAMS   SOF_CMD_TYPE(0x001)
#define SOF_IPC_STREAM_PCM_FREE     SOF_CMD_TYPE(0x003)
#define SOF_IPC_STREAM_TRIG_START   SOF_CMD_TYPE(0x004)
#define SOF_IPC_STREAM_TRIG_STOP    SOF_CMD_TYPE(0x005)

C_ASSERT(sizeof(struct sof_ipc_stream_params) == 84);
C_ASSERT(sizeof(struct sof_ipc_pcm_params) == 108);
C_ASSERT(sizeof(struct sof_ipc_fw_version) == 60);
C_ASSERT(sizeof(struct sof_ipc_window_elem) == 24);
