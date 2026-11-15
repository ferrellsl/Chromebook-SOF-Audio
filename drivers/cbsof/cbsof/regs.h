// Intel cAVS 1.5 (Apollo Lake / Gemini Lake) audio DSP registers.
// Offsets and bit meanings follow the Linux SOF driver (sound/soc/sof/intel/hda.h),
// used under its BSD-3-Clause license. Copyright(c) 2017 Intel Corporation.
#pragma once

// ---- Processing pipe capability (PPCAP), offsets from the ppcap pointer ----
#define PP_PPCTL                0x04
#define PP_PPSTS                0x08
#define PPCTL_PIE               (1u << 31)
#define PPCTL_GPROCEN           (1u << 30)

// ---- ADSP BAR (BAR4) ----
#define DSP_REG_ADSPCS          0x04
#define DSP_REG_ADSPIC          0x08
#define DSP_REG_ADSPIS          0x0C

#define ADSPCS_CRST(cm)         ((ULONG)(cm) << 0)
#define ADSPCS_CSTALL(cm)       ((ULONG)(cm) << 8)
#define ADSPCS_SPA(cm)          ((ULONG)(cm) << 16)
#define ADSPCS_CPA(cm)          ((ULONG)(cm) << 24)

#define ADSPIC_IPC              (1u << 0)
#define ADSPIS_IPC              (1u << 0)
#define ADSPIS_CL_DMA           (1u << 1)

#define DSP_IPC_BASE            0x40
#define DSP_REG_HIPCT           (DSP_IPC_BASE + 0x00)   // DSP -> host doorbell
#define DSP_REG_HIPCTE          (DSP_IPC_BASE + 0x04)
#define DSP_REG_HIPCI           (DSP_IPC_BASE + 0x08)   // host -> DSP doorbell
#define DSP_REG_HIPCIE          (DSP_IPC_BASE + 0x0C)
#define DSP_REG_HIPCCTL         (DSP_IPC_BASE + 0x10)

#define HIPCI_BUSY              (1u << 31)
#define HIPCI_MSG_MASK          0x7FFFFFFF
#define HIPCIE_DONE             (1u << 30)
#define HIPCIE_MSG_MASK         0x3FFFFFFF
#define HIPCT_BUSY              (1u << 31)
#define HIPCT_MSG_MASK          0x7FFFFFFF
#define HIPCTE_MSG_MASK         0x3FFFFFFF
#define HIPCCTL_DONE            (1u << 1)
#define HIPCCTL_BUSY            (1u << 0)

// SRAM windows inside BAR4
#define SRAM_WINDOW_OFFSET(x)   (0x80000 + (x) * 0x20000)
#define DSP_MBOX_OFFSET         SRAM_WINDOW_OFFSET(0)
#define DSP_SRAM_ROM_STATUS     (DSP_MBOX_OFFSET + 0x0)
#define DSP_SRAM_ROM_ERROR      (DSP_MBOX_OFFSET + 0x4)
#define DSP_MBOX_UPLINK_OFFSET  0x81000                 // FW_READY is written here

// ROM / firmware status register fields
#define FSR_STATE_MASK          0x00FFFFFF
#define FSR_TO_STATE(x)         ((x) & FSR_STATE_MASK)
#define FSR_STATE_INIT_DONE     0x1
#define FSR_STATE_FW_ENTERED    0x5

// ROM IPC
#define ROM_IPC_CONTROL         0x01000000
#define ROM_IPC_PURGE_FW        0x00004000

// Timeouts (microseconds), same as Linux
#define DSP_RESET_TIMEOUT_US    50000
#define DSP_PD_TIMEOUT_US       50000
#define DSP_INIT_TIMEOUT_US     500000
#define DSP_ROM_INIT_TIMEOUT_US 150000
#define DSP_BASEFW_TIMEOUT_US   3000000
#define DSP_FW_BOOT_ATTEMPTS    3

// Gemini Lake has 2 DSP cores; core 0 boots the firmware.
#define GLK_CORE_MASK_ALL       0x3
#define GLK_CORE_MASK_INIT      0x1

// SSP ports inside BAR4 (APL/GLK)
#define GLK_SSP_BASE_OFFSET     0x2000
#define GLK_SSP_COUNT           6
#define SSP_DEV_MEM_SIZE        0x1000
#define SSP_SSC1_OFFSET         0x4
#define SSP_SET_CBP_CFP         ((1u << 25) | (1u << 24))   // SCLK + SFRM consumer

// HDA controller registers (HDA BAR / BAR0)
#define HDA_REG_GCAP            0x00
#define HDA_REG_LLCH            0x14        // offset of the first linked capability
#define HDA_CAP_HDR_ID(x)       (((x) >> 16) & 0xFFF)
#define HDA_CAP_HDR_NEXT(x)     ((x) & 0xFFFF)
#define HDA_PP_CAP_ID           0x3
#define HDA_MAX_CAPS            10
#define HDA_SD_BASE             0x80
#define HDA_SD_SIZE             0x20
#define HDA_SD_FORMAT           0x12
#define PPCTL_PROCEN(idx)       (1u << (idx))

// HDA BDL entry
#pragma pack(push, 1)
typedef struct _HDA_BDLE {
    ULONG AddrLo;
    ULONG AddrHi;
    ULONG Size;
    ULONG Ioc;
} HDA_BDLE, *PHDA_BDLE;
#pragma pack(pop)
