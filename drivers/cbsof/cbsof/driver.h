// cbsof: open SOF audio DSP driver for Gemini Lake Chromebooks.
// Internal header for the DSP side (boot, IPC, pipelines). The PortCls side
// only sees sofapi.h.
#pragma once

#include <ntddk.h>
#include <hdaudio.h>
#include <initguid.h>

#include "regs.h"
#include "sof_ipc3.h"
#include "sofapi.h"

typedef int BOOL; // the bus interface uses a Win32 BOOL (int)

#define CBSOF_POOL_TAG 'fSbC'
#define CBSOF_FW_PATH  L"\\SystemRoot\\System32\\drivers\\cbsof\\sof-glk.ri"

// ---------------------------------------------------------------------------
// ADSP bus interface exported by sklhdaudbus (adsp.h in coolstar/sklhdaudbus,
// BSD-3-Clause). The layout must match the bus driver exactly.
// ---------------------------------------------------------------------------
// {752A2CAE-3455-4D18-A184-8B34B22632CE}
DEFINE_GUID(GUID_ADSP_BUS_INTERFACE,
    0x752a2cae, 0x3455, 0x4d18, 0xa1, 0x84, 0x8b, 0x34, 0xb2, 0x26, 0x32, 0xce);

typedef struct _CB_PCI_BAR {
    PVOID Base;
    ULONG Len;
} CB_PCI_BAR, *PCB_PCI_BAR;

typedef struct _NHLT_INFO {
    PVOID nhlt;
    UINT64 nhltSz;
} NHLT_INFO, *PNHLT_INFO;

typedef NTSTATUS(*PGET_ADSP_RESOURCES)(PVOID ctx, CB_PCI_BAR* hdaBar, CB_PCI_BAR* adspBar, PVOID* ppcap, PNHLT_INFO nhltInfo, BUS_INTERFACE_STANDARD* pciConfig);
typedef NTSTATUS(*PDSP_SET_POWER_STATE)(PVOID ctx, DEVICE_POWER_STATE newPowerState);
typedef BOOL(*PADSP_INTERRUPT_CALLBACK)(PVOID context);
typedef NTSTATUS(*PREGISTER_ADSP_INTERRUPT)(PVOID ctx, PADSP_INTERRUPT_CALLBACK callback, PVOID callbackContext);
typedef NTSTATUS(*PUNREGISTER_ADSP_INTERRUPT)(PVOID ctx);
typedef NTSTATUS(*PGET_STREAM)(PVOID ctx, HDAUDIO_STREAM_FORMAT StreamFormat, PHANDLE Handle, UINT8* streamTag);
typedef NTSTATUS(*PFREE_STREAM)(PVOID ctx, HANDLE Handle);
typedef NTSTATUS(*PDSP_PREPARE_STREAM)(PVOID ctx, HANDLE Handle, unsigned int ByteSize, int frags, PVOID* bdlBuf);
typedef NTSTATUS(*PDSP_CLEANUP_STREAM)(PVOID ctx, HANDLE Handle);
typedef UINT32(*PDSP_STREAM_POSITION)(PVOID ctx, HANDLE Handle);
typedef void (*PDSP_START_STOP_STREAM)(PVOID ctx, HANDLE Handle, BOOL startStop);
typedef void (*PDSP_ENABLE_SPIB)(PVOID ctx, HANDLE Handle, UINT32 value);
typedef void (*PDSP_DISABLE_SPIB)(PVOID ctx, HANDLE Handle);

typedef struct _ADSP_BUS_INTERFACE {
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;

    UINT16 CtlrDevId;
    PGET_ADSP_RESOURCES GetResources;
    PDSP_SET_POWER_STATE SetDSPPowerState;
    PREGISTER_ADSP_INTERRUPT RegisterInterrupt;
    PUNREGISTER_ADSP_INTERRUPT UnregisterInterrupt;
    PGET_STREAM GetRenderStream;
    PGET_STREAM GetCaptureStream;
    PFREE_STREAM FreeStream;
    PDSP_PREPARE_STREAM PrepareDSP;
    PDSP_CLEANUP_STREAM CleanupDSP;
    PDSP_START_STOP_STREAM TriggerDSP;
    PDSP_STREAM_POSITION StreamPosition;
    PDSP_ENABLE_SPIB DSPEnableSPIB;
    PDSP_DISABLE_SPIB DSPDisableSPIB;
} ADSP_BUS_INTERFACE, *PADSP_BUS_INTERFACE;

// ---------------------------------------------------------------------------
// Device context
// ---------------------------------------------------------------------------
#define CBSOF_LOG_SIZE 8192

typedef struct _CBSOF_CONTEXT {
    PDEVICE_OBJECT Fdo;
    PDEVICE_OBJECT Pdo;
    ADSP_BUS_INTERFACE Bus;
    BOOLEAN BusValid;
    BOOLEAN InterruptRegistered;

    PUCHAR HdaBar;
    ULONG HdaBarLen;
    PUCHAR DspBar;
    ULONG DspBarLen;
    PUCHAR PpCap;

    // IPC state shared between the bus ISR callback (DIRQL), our DPC and
    // passive code. The ISR only latches doorbell values with Interlocked ops
    // and masks the doorbell interrupt; the DPC does the rest.
    KDPC IpcDpc;
    volatile LONG HipctLatched;  // 1 = DSP->host message pending
    volatile LONG HipctMsg;
    volatile LONG HipcteMsg;
    volatile LONG ReplyLatched;  // 1 = DSP replied to our host->DSP message
    volatile LONG ReplyMsg;
    volatile LONG PanicCode;     // non-zero if the firmware reported a panic
    volatile LONG IrqCount;
    KEVENT FwReadyEvent;
    KEVENT ReplyEvent;

    BOOLEAN FwReady;
    ULONG HostBoxOffset;         // host -> DSP mailbox (BAR4 offset)
    ULONG HostBoxSize;
    ULONG DspBoxOffset;          // DSP -> host mailbox
    ULONG DspBoxSize;
    ULONG StreamBoxOffset;
    ULONG StreamBoxSize;
    struct sof_ipc_fw_ready FwReadyMsg;
    UCHAR ReplyBuf[SOF_IPC_MSG_MAX_SIZE];

    // Serializes host->DSP IPC between streams, power and setup paths
    KMUTEX IpcMutex;

    // Coolstar CsAudio callback API (codec drivers)
    PCALLBACK_OBJECT CsAudioCallbackObj;
    PVOID CsAudioRegistration;
    volatile LONG CodecEndpoints;   // bit per CSAudioEndpointType that registered

    // Pipelines loaded from the topology blob
    BOOLEAN TopologyLoaded;
    UINT32 SpeakerHostComp;

    BOOLEAN DspRunning;
    BOOLEAN GuardTripped;   // previous power-up crashed; DSP stays off
    ULONG Instance;     // changes on every SofDeviceStart (bus resources may be new)
    ULONG BootId;       // changes on every successful DSP boot (pipelines are new)

    // Bring-up log, copied to the registry so it can be read without a debugger
    KSPIN_LOCK LogLock;
    CHAR Log[CBSOF_LOG_SIZE];
    ULONG LogLen;
} CBSOF_CONTEXT;

// sofdevice.cpp
void CbLog(PCBSOF_CONTEXT ctx, const char* fmt, ...);
void CbFlushLog(PCBSOF_CONTEXT ctx);

// dsp.cpp
NTSTATUS DspBoot(PCBSOF_CONTEXT ctx);
void DspShutdown(PCBSOF_CONTEXT ctx);
NTSTATUS DspIpcTest(PCBSOF_CONTEXT ctx);
NTSTATUS CbLoadFile(PCBSOF_CONTEXT ctx, PCWSTR filePath, PUCHAR* data, ULONG* size);
NTSTATUS IpcTx(PCBSOF_CONTEXT ctx, const void* msg, ULONG len, ULONG timeoutMs);
NTSTATUS IpcTxCheck(PCBSOF_CONTEXT ctx, const void* msg, ULONG len, const char* what);
ULONG DspReadReg(PCBSOF_CONTEXT ctx, ULONG off);

// sofpcm.cpp
NTSTATUS CsAudioOpen(PCBSOF_CONTEXT ctx);
void CsAudioClose(PCBSOF_CONTEXT ctx);
NTSTATUS SofLoadTopology(PCBSOF_CONTEXT ctx);
BOOL DspInterruptCallback(PVOID context);
KDEFERRED_ROUTINE DspIpcDpc;
