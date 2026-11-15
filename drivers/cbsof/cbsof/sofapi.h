// cbsof: interface between the PortCls endpoints (pc\) and the SOF DSP side.
// Must compile with either <portcls.h> or <ntddk.h> already included.
#pragma once

typedef struct _CBSOF_CONTEXT CBSOF_CONTEXT, *PCBSOF_CONTEXT;

// SOF PCM ids from the topology (sof-glk-da7219.tplg)
#define SOF_PCM_SPEAKERS    0
#define SOF_PCM_HEADSET     1
#define SOF_PCM_DMIC        99

// One host stream through a SOF pipeline.
typedef struct _SOF_PCM {
    ULONG Instance;         // DSP instance the HDA stream belongs to
    ULONG BootId;           // DSP boot the PCM_PARAMS were sent to
    ULONG Channels;
    ULONG Rate;
    ULONG BitsPerSample;
    ULONG PcmId;
    BOOLEAN Capture;
    UINT32 HostComp;
    HANDLE Stream;          // HDA stream from sklhdaudbus
    UCHAR StreamTag;
    PMDL Mdl;
    ULONG BufferBytes;
    PVOID PageTable;
    PHYSICAL_ADDRESS PageTablePhys;
    BOOLEAN ParamsSet;
    BOOLEAN DmaRunning;
    BOOLEAN Started;        // TRIG_START sent at least once since PCM_PARAMS
    BOOLEAN Paused;
    BOOLEAN AmpOn;
} SOF_PCM, *PSOF_PCM;

// The single DSP instance (one audio controller per machine)
extern PCBSOF_CONTEXT g_Cbsof;

// sofdevice.cpp: called from CAdapterCommon
NTSTATUS SofDeviceStart(PDEVICE_OBJECT Fdo);
void SofDeviceStop(void);
void SofDevicePowerDown(void);
NTSTATUS SofDevicePowerUp(void);

// sofpcm.cpp: called from the WaveRT stream (PASSIVE_LEVEL except Position)
NTSTATUS SofPcmOpen(PSOF_PCM Pcm, ULONG PcmId, BOOLEAN Capture, PMDL Mdl, ULONG BufferBytes,
                    ULONG Channels, ULONG Rate, ULONG BitsPerSample);
NTSTATUS SofPcmRun(PSOF_PCM Pcm);
NTSTATUS SofPcmPause(PSOF_PCM Pcm);
void SofPcmClose(PSOF_PCM Pcm);
ULONG SofPcmPosition(PSOF_PCM Pcm);     // byte offset in the buffer, any IRQL <= DISPATCH
