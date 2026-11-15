// cbsof: speaker pipeline loading and SOF PCM streams for the WaveRT endpoints.
//
// The pipeline messages come from tools/tplg2ipc.py (spk.cbtp). Stream setup
// follows Linux sof_ipc3_pcm_hw_params / hda_dsp_pcm_hw_params / sof_pcm_trigger
// (BSD-3-Clause). The codec drivers are told about the stream through
// Coolstar's CsAudio callback API, as used by coolstar/max98357a (Apache-2.0).
#include "driver.h"
#include <ntstrsafe.h>

// Outputs first (required); the mics are loaded after them, so a problem
// there never costs the speakers and headphones.
#define CBSOF_TPLG_PATH     L"\\SystemRoot\\System32\\drivers\\cbsof\\spk.cbtp"
#define CBSOF_TPLG_MIC_PATH  L"\\SystemRoot\\System32\\drivers\\cbsof\\mic.cbtp"
#define CBSOF_TPLG_DMIC_PATH L"\\SystemRoot\\System32\\drivers\\cbsof\\dmic.cbtp"

// ---------------------------------------------------------------------------
// CsAudio callback API (\Callback\CsAudioCallbackAPI)
// ---------------------------------------------------------------------------
typedef enum {
    CSAudioEndpointTypeDSP,
    CSAudioEndpointTypeSpeaker,
    CSAudioEndpointTypeHeadphone,
    CSAudioEndpointTypeMicArray,
    CSAudioEndpointTypeMicJack
} CSAudioEndpointType;

typedef enum {
    CSAudioEndpointRegister,
    CSAudioEndpointStart,
    CSAudioEndpointStop,
    CSAudioEndpointOverrideFormat
} CSAudioEndpointRequest;

typedef struct CSAUDIOFORMATOVERRIDE {
    UINT16 channels;
    UINT16 frequency;
    UINT16 bitsPerSample;
    UINT16 validBitsPerSample;
    BOOL force32BitOutputContainer;
} CsAudioFormatOverride;

typedef struct CSAUDIOARG {
    UINT32 argSz;
    CSAudioEndpointType endpointType;
    CSAudioEndpointRequest endpointRequest;
    union {
        CsAudioFormatOverride formatOverride;
    };
} CsAudioArg, *PCsAudioArg;

static int g_CbsofArg2 = 2;   // identifies our own notifications

static VOID CsAudioCallback(PVOID CallbackContext, PVOID Argument1, PVOID Argument2)
{
    PCBSOF_CONTEXT ctx = (PCBSOF_CONTEXT)CallbackContext;
    if (!ctx || !Argument1 || Argument2 == &g_CbsofArg2) {
        return;
    }
    PCsAudioArg arg = (PCsAudioArg)Argument1;
    if (arg->argSz < FIELD_OFFSET(CsAudioArg, formatOverride)) {
        return;
    }
    if (arg->endpointRequest == CSAudioEndpointRegister && (ULONG)arg->endpointType < 8) {
        InterlockedOr(&ctx->CodecEndpoints, 1L << arg->endpointType);
    }
}

NTSTATUS CsAudioOpen(PCBSOF_CONTEXT ctx)
{
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"\\CallBack\\CsAudioCallbackAPI");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
                               NULL, NULL);
    NTSTATUS status = ExCreateCallback(&ctx->CsAudioCallbackObj, &oa, TRUE, TRUE);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "ExCreateCallback(CsAudioCallbackAPI) failed 0x%08X", status);
        return status;
    }
    ctx->CsAudioRegistration = ExRegisterCallback(ctx->CsAudioCallbackObj, CsAudioCallback, ctx);
    return STATUS_SUCCESS;
}

void CsAudioClose(PCBSOF_CONTEXT ctx)
{
    if (ctx->CsAudioRegistration) {
        ExUnregisterCallback(ctx->CsAudioRegistration);
        ctx->CsAudioRegistration = NULL;
    }
    if (ctx->CsAudioCallbackObj) {
        ObDereferenceObject(ctx->CsAudioCallbackObj);
        ctx->CsAudioCallbackObj = NULL;
    }
}

static void CsAudioNotify(PCBSOF_CONTEXT ctx, CSAudioEndpointType type, CSAudioEndpointRequest req)
{
    if (!ctx->CsAudioCallbackObj) {
        return;
    }
    CsAudioArg arg;
    RtlZeroMemory(&arg, sizeof(arg));
    arg.argSz = sizeof(arg);
    arg.endpointType = type;
    arg.endpointRequest = req;
    ExNotifyCallback(ctx->CsAudioCallbackObj, &arg, &g_CbsofArg2);
}

// ---------------------------------------------------------------------------
// Topology replay
// ---------------------------------------------------------------------------
#pragma pack(push, 4)
typedef struct _CBTP_HEADER {
    UINT32 magic;       // "CBTP"
    UINT32 version;
    UINT32 numMsgs;
    UINT32 numPcms;
} CBTP_HEADER;

typedef struct _CBTP_PCM {
    UINT32 pcmId;
    UINT32 direction;
    UINT32 hostCompId;
    UINT32 pipelineId;
    CHAR name[32];
} CBTP_PCM;
#pragma pack(pop)

static NTSTATUS LoadTopology(PCBSOF_CONTEXT ctx, PCWSTR path, const char* label)
{
    PUCHAR blob = NULL;
    ULONG size = 0;
    NTSTATUS status = CbLoadFile(ctx, path, &blob, &size);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    const CBTP_HEADER* hdr = (const CBTP_HEADER*)blob;
    if (size < sizeof(*hdr) || hdr->magic != 'PTBC' || hdr->version != 1 ||
        hdr->numPcms > 16 || size < sizeof(*hdr) + hdr->numPcms * sizeof(CBTP_PCM)) {
        CbLog(ctx, "bad topology blob");
        ExFreePoolWithTag(blob, CBSOF_POOL_TAG);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    const CBTP_PCM* pcms = (const CBTP_PCM*)(blob + sizeof(*hdr));

    ULONG off = sizeof(*hdr) + hdr->numPcms * sizeof(CBTP_PCM);
    for (ULONG i = 0; i < hdr->numMsgs; i++) {
        if (off + 4 > size) {
            status = STATUS_INVALID_IMAGE_FORMAT;
            break;
        }
        ULONG len = *(const UINT32*)(blob + off);
        off += 4;
        if (len < sizeof(sof_ipc_cmd_hdr) || len > SOF_IPC_MSG_MAX_SIZE || off + len > size) {
            status = STATUS_INVALID_IMAGE_FORMAT;
            break;
        }
        CHAR what[24];
        RtlStringCbPrintfA(what, sizeof(what), "tplg msg %u", i);
        status = IpcTxCheck(ctx, blob + off, len, what);
        if (!NT_SUCCESS(status)) {
            break;
        }
        off += len;
    }
    if (NT_SUCCESS(status)) {
        // Host component per (PCM id, direction), e.g. Speakers = (0, play);
        // added only once all of this file's pipelines are in place.
        CbLogV(ctx, "topology (%s): %u messages accepted", label, hdr->numMsgs);
        for (ULONG i = 0; i < hdr->numPcms && ctx->NumPcms < CBSOF_MAX_PCMS; i++) {
            ctx->Pcms[ctx->NumPcms].PcmId = pcms[i].pcmId;
            ctx->Pcms[ctx->NumPcms].Capture = pcms[i].direction != 0;
            ctx->Pcms[ctx->NumPcms].HostComp = pcms[i].hostCompId;
            CbLogV(ctx, "  PCM %u %s -> host comp %u", pcms[i].pcmId,
                   pcms[i].direction ? "capture" : "playback", pcms[i].hostCompId);
            ctx->NumPcms++;
        }
    }
    ExFreePoolWithTag(blob, CBSOF_POOL_TAG);
    return status;
}

// ---------------------------------------------------------------------------
// HDA helpers
// ---------------------------------------------------------------------------
// Apollo/Gemini Lake: write SDxFMT with the stream coupled (PROCEN = 0),
// then decouple again (SOF_INTEL_PROCEN_FMT_QUIRK in Linux hda-stream.c).
static void ApplyProcenFormatQuirk(PCBSOF_CONTEXT ctx, UINT8 tag, BOOLEAN playback)
{
    USHORT gcap = READ_REGISTER_USHORT((volatile USHORT*)(ctx->HdaBar + HDA_REG_GCAP));
    ULONG iss = (gcap >> 8) & 0xF;
    ULONG idx = playback ? iss + tag - 1 : (ULONG)tag - 1;
    volatile USHORT* fmtReg = (volatile USHORT*)(ctx->HdaBar + HDA_SD_BASE + idx * HDA_SD_SIZE + HDA_SD_FORMAT);
    volatile ULONG* ppctl = (volatile ULONG*)(ctx->PpCap + PP_PPCTL);

    USHORT fmt = READ_REGISTER_USHORT(fmtReg);
    WRITE_REGISTER_ULONG(ppctl, READ_REGISTER_ULONG(ppctl) & ~PPCTL_PROCEN(idx));
    WRITE_REGISTER_USHORT(fmtReg, fmt);
    WRITE_REGISTER_ULONG(ppctl, READ_REGISTER_ULONG(ppctl) | PPCTL_PROCEN(idx));
    CbLog(ctx, "stream tag %u -> SD index %u (GCAP 0x%04X), format 0x%04X", tag, idx, gcap, fmt);
}

// ---------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------
// Host component for a PCM, or MAXULONG if the topology has none
static UINT32 FindHostComp(PCBSOF_CONTEXT ctx, ULONG pcmId, BOOLEAN capture)
{
    for (ULONG i = 0; i < ctx->NumPcms; i++) {
        if (ctx->Pcms[i].PcmId == pcmId && ctx->Pcms[i].Capture == capture) {
            return ctx->Pcms[i].HostComp;
        }
    }
    return MAXULONG;
}

NTSTATUS SofLoadTopology(PCBSOF_CONTEXT ctx)
{
    ctx->CodecEndpoints = 0;
    CsAudioNotify(ctx, CSAudioEndpointTypeDSP, CSAudioEndpointRegister);
    CbLogV(ctx, "codec endpoints registered: mask 0x%X (bit1 = speaker amp)", ctx->CodecEndpoints);

    ctx->NumPcms = 0;
    NTSTATUS status = LoadTopology(ctx, CBSOF_TPLG_PATH, "outputs");
    ctx->TopologyLoaded = NT_SUCCESS(status) && FindHostComp(ctx, SOF_PCM_SPEAKERS, FALSE) != MAXULONG;
    if (!ctx->TopologyLoaded) {
        CbLog(ctx, "topology load failed 0x%08X", status);
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }
    // Each mic on its own, so one failing never costs the other (or the outputs)
    NTSTATUS micStatus = LoadTopology(ctx, CBSOF_TPLG_MIC_PATH, "headset mic");
    if (!NT_SUCCESS(micStatus)) {
        CbLog(ctx, "headset mic pipeline not loaded (0x%08X)", micStatus);
    }
    micStatus = LoadTopology(ctx, CBSOF_TPLG_DMIC_PATH, "built-in mics");
    if (!NT_SUCCESS(micStatus)) {
        CbLog(ctx, "built-in mic pipeline not loaded (0x%08X)", micStatus);
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// PCM streams (Linux sof_pcm_hw_params / sof_pcm_trigger order)
// ---------------------------------------------------------------------------
static NTSTATUS SendStreamCmd(PCBSOF_CONTEXT ctx, PSOF_PCM pcm, ULONG cmd, const char* what)
{
    sof_ipc_stream msg;
    RtlZeroMemory(&msg, sizeof(msg));
    msg.hdr.size = sizeof(msg);
    msg.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | cmd;
    msg.comp_id = pcm->HostComp;
    return IpcTxCheck(ctx, &msg, sizeof(msg), what);
}

// A PCM belongs to one DSP instance; after a stop/restart of the device its
// HDA stream handle points into the old bus state and must not be used.
static BOOLEAN PcmUsable(PCBSOF_CONTEXT ctx, PSOF_PCM pcm)
{
    return ctx != NULL && pcm->Instance != 0 && pcm->Instance == ctx->Instance;
}

// STREAM_PCM_PARAMS for an already prepared HDA stream (also after a DSP reboot)
static NTSTATUS SendPcmParams(PCBSOF_CONTEXT ctx, PSOF_PCM Pcm)
{
    sof_ipc_pcm_params pcm;
    ULONG bytesPerSample = Pcm->BitsPerSample / 8;

    ApplyProcenFormatQuirk(ctx, Pcm->StreamTag, !Pcm->Capture);

    RtlZeroMemory(&pcm, sizeof(pcm));
    pcm.hdr.size = sizeof(pcm);
    pcm.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_PARAMS;
    pcm.comp_id = Pcm->HostComp;
    pcm.params.hdr.size = sizeof(pcm.params);
    pcm.params.buffer.hdr.size = sizeof(pcm.params.buffer);
    pcm.params.buffer.phy_addr = Pcm->PageTablePhys.LowPart;
    pcm.params.buffer.pages = Pcm->BufferBytes / PAGE_SIZE;
    pcm.params.buffer.size = Pcm->BufferBytes;
    pcm.params.direction = Pcm->Capture ? 1 : 0;
    pcm.params.frame_fmt = 0;            // S16_LE
    pcm.params.buffer_fmt = 0;           // interleaved
    pcm.params.rate = Pcm->Rate;
    pcm.params.stream_tag = Pcm->StreamTag;
    pcm.params.channels = (UINT16)Pcm->Channels;
    pcm.params.sample_valid_bytes = (UINT16)bytesPerSample;
    pcm.params.sample_container_bytes = (UINT16)bytesPerSample;
    pcm.params.host_period_bytes = min(Pcm->BufferBytes / 2, Pcm->Rate / 100 * Pcm->Channels * bytesPerSample);
    pcm.params.no_stream_position = 1;   // HDA: positions from DPIB
    NTSTATUS status = IpcTxCheck(ctx, &pcm, sizeof(pcm), "PCM_PARAMS");
    if (NT_SUCCESS(status)) {
        Pcm->ParamsSet = TRUE;
        Pcm->BootId = ctx->BootId;
    }
    return status;
}

NTSTATUS SofPcmOpen(PSOF_PCM Pcm, ULONG PcmId, BOOLEAN Capture, PMDL Mdl, ULONG BufferBytes,
                    ULONG Channels, ULONG Rate, ULONG BitsPerSample)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    NTSTATUS status;
    PHYSICAL_ADDRESS low = {}, high = {}, boundary = {};
    PVOID bdlVa = NULL;
    HDAUDIO_STREAM_FORMAT fmt = {};
    ULONG pages;
    PPFN_NUMBER pfns;

    RtlZeroMemory(Pcm, sizeof(*Pcm));
    Pcm->PcmId = PcmId;
    Pcm->Capture = Capture;

    if (!ctx || !ctx->DspRunning || !ctx->TopologyLoaded) {
        return STATUS_DEVICE_NOT_READY;
    }
    Pcm->Instance = ctx->Instance;
    UINT32 hostComp = FindHostComp(ctx, PcmId, Capture);
    if (hostComp == MAXULONG) {
        CbLog(ctx, "SofPcmOpen: no %s pipeline for PCM %u", Capture ? "capture" : "playback", PcmId);
        return STATUS_NOT_SUPPORTED;
    }
    // The topology's PCMs are 48 kHz; DMIC capture is 4 channels, the rest stereo.
    if (!Mdl || BufferBytes == 0 || (BufferBytes & (PAGE_SIZE - 1)) ||
        BitsPerSample != 16 || Channels != (PcmId == SOF_PCM_DMIC ? 4u : 2u) || Rate != 48000) {
        CbLog(ctx, "SofPcmOpen: unsupported buffer/format (%u bytes, %u ch, %u Hz, %u bits)",
              BufferBytes, Channels, Rate, BitsPerSample);
        return STATUS_INVALID_PARAMETER;
    }
    pages = BufferBytes / PAGE_SIZE;
    if (pages > 256) {
        return STATUS_INVALID_BUFFER_SIZE;    // HDA BDL limit
    }
    Pcm->HostComp = hostComp;
    Pcm->Mdl = Mdl;
    Pcm->BufferBytes = BufferBytes;

    fmt.SampleRate = Rate;
    fmt.ValidBitsPerSample = (USHORT)BitsPerSample;
    fmt.ContainerSize = (USHORT)BitsPerSample;
    fmt.NumberOfChannels = (USHORT)Channels;
    status = Capture
        ? ctx->Bus.GetCaptureStream(ctx->Bus.Context, fmt, &Pcm->Stream, &Pcm->StreamTag)
        : ctx->Bus.GetRenderStream(ctx->Bus.Context, fmt, &Pcm->Stream, &Pcm->StreamTag);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "Get%sStream failed 0x%08X", Capture ? "Capture" : "Render", status);
        Pcm->Stream = NULL;
        return status;
    }

    high.QuadPart = 0xFFFFFFFF;
    Pcm->PageTable = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, low, high, boundary, MmNonCached);
    if (!Pcm->PageTable) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    Pcm->PageTablePhys = MmGetPhysicalAddress(Pcm->PageTable);

    // BDL: one entry per page of the WaveRT buffer
    status = ctx->Bus.PrepareDSP(ctx->Bus.Context, Pcm->Stream, BufferBytes, (int)pages, &bdlVa);
    if (!NT_SUCCESS(status) || !bdlVa) {
        CbLog(ctx, "PrepareDSP failed 0x%08X", status);
        if (NT_SUCCESS(status)) status = STATUS_UNSUCCESSFUL;
        goto fail;
    }
    pfns = MmGetMdlPfnArray(Mdl);
    {
        PHDA_BDLE bdl = (PHDA_BDLE)bdlVa;
        PUCHAR pt = (PUCHAR)Pcm->PageTable;
        RtlZeroMemory(pt, PAGE_SIZE);
        for (ULONG i = 0; i < pages; i++) {
            ULONGLONG a = (ULONGLONG)pfns[i] << PAGE_SHIFT;
            bdl[i].AddrLo = (ULONG)a;
            bdl[i].AddrHi = (ULONG)(a >> 32);
            bdl[i].Size = PAGE_SIZE;
            bdl[i].Ioc = 0;

            // compressed 20-bit PFN page table (snd_sof_create_page_table)
            ULONG pfn = (ULONG)pfns[i];
            PUCHAR p = pt + ((5 * i) >> 1);
            ULONG v = (i & 1) ? ((p[0] & 0xF) | (pfn << 4)) : pfn;
            p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); p[2] = (UCHAR)(v >> 16); p[3] = (UCHAR)(v >> 24);
        }
        KeMemoryBarrier();
    }

    Pcm->Channels = Channels;
    Pcm->Rate = Rate;
    Pcm->BitsPerSample = BitsPerSample;
    if (Capture) {
        Pcm->BufVa = (PSHORT)MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority | MdlMappingNoExecute);
    }

    status = SendPcmParams(ctx, Pcm);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }
    CbLog(ctx, "PCM %u %s open: %u ch, %u bytes (%u pages), tag %u", PcmId, Capture ? "capture" : "playback",
          Channels, BufferBytes, pages, Pcm->StreamTag);
    return STATUS_SUCCESS;

fail:
    SofPcmClose(Pcm);
    return status;
}

NTSTATUS SofPcmRun(PSOF_PCM Pcm)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (!PcmUsable(ctx, Pcm) || !ctx->DspRunning || !Pcm->ParamsSet) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Pcm->DmaRunning) {
        return STATUS_SUCCESS;
    }
    if (Pcm->BootId != ctx->BootId) {
        // The DSP was power-cycled while this stream was paused: its pipeline
        // state is gone, so set the stream up again before starting it.
        CbLog(ctx, "PCM %u: DSP rebooted, re-sending PCM_PARAMS", Pcm->PcmId);
        Pcm->Started = FALSE;
        NTSTATUS st = SendPcmParams(ctx, Pcm);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }
    // The MAX98357A speaker amp is switched by the CsAudio API; the DA7219
    // headphone path is configured by its own driver and needs no call.
    if (!Pcm->AmpOn && Pcm->PcmId == SOF_PCM_SPEAKERS) {
        CsAudioNotify(ctx, CSAudioEndpointTypeSpeaker, CSAudioEndpointStart);
        Pcm->AmpOn = TRUE;
    }
    // Start/release: host DMA first, then the firmware
    ctx->Bus.TriggerDSP(ctx->Bus.Context, Pcm->Stream, TRUE);
    Pcm->DmaRunning = TRUE;
    NTSTATUS status = Pcm->Started
        ? SendStreamCmd(ctx, Pcm, SOF_IPC_STREAM_TRIG_RELEASE, "TRIG_RELEASE")
        : SendStreamCmd(ctx, Pcm, SOF_IPC_STREAM_TRIG_START, "TRIG_START");
    if (NT_SUCCESS(status)) {
        Pcm->Started = TRUE;
        Pcm->Paused = FALSE;
    }
    return status;
}

NTSTATUS SofPcmPause(PSOF_PCM Pcm)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (!PcmUsable(ctx, Pcm) || !Pcm->DmaRunning) {
        return STATUS_SUCCESS;
    }
    // Pause: firmware first, then host DMA
    NTSTATUS status = (ctx->DspRunning && Pcm->BootId == ctx->BootId)
        ? SendStreamCmd(ctx, Pcm, SOF_IPC_STREAM_TRIG_PAUSE, "TRIG_PAUSE")
        : STATUS_SUCCESS;
    ctx->Bus.TriggerDSP(ctx->Bus.Context, Pcm->Stream, FALSE);
    Pcm->DmaRunning = FALSE;
    Pcm->Paused = TRUE;
    if (Pcm->AmpOn) {
        CsAudioNotify(ctx, CSAudioEndpointTypeSpeaker, CSAudioEndpointStop);
        Pcm->AmpOn = FALSE;
    }
    return status;
}

void SofPcmClose(PSOF_PCM Pcm)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (Pcm->Capture && Pcm->BufVa && Pcm->Started && ctx) {
        // Peak per channel over the whole recording, in percent of full scale
        CbLog(ctx, "PCM %u capture peaks (%% of full scale): %u %u %u %u", Pcm->PcmId,
              Pcm->Peak[0] * 100 / 32768, Pcm->Peak[1] * 100 / 32768,
              Pcm->Peak[2] * 100 / 32768, Pcm->Peak[3] * 100 / 32768);
        Pcm->BufVa = NULL;
    }
    if (!PcmUsable(ctx, Pcm)) {
        // The DSP instance this stream belonged to is gone: never touch the
        // old HDA stream. Only free our own memory.
        if (Pcm->PageTable) {
            MmFreeContiguousMemorySpecifyCache(Pcm->PageTable, PAGE_SIZE, MmNonCached);
            Pcm->PageTable = NULL;
        }
        Pcm->Stream = NULL;
        Pcm->ParamsSet = Pcm->Started = Pcm->DmaRunning = Pcm->AmpOn = FALSE;
        return;
    }
    BOOLEAN fwHasStream = ctx->DspRunning && Pcm->BootId == ctx->BootId;
    if (Pcm->Started && fwHasStream) {
        SendStreamCmd(ctx, Pcm, SOF_IPC_STREAM_TRIG_STOP, "TRIG_STOP");
    }
    if (Pcm->DmaRunning) {
        ctx->Bus.TriggerDSP(ctx->Bus.Context, Pcm->Stream, FALSE);
        Pcm->DmaRunning = FALSE;
    }
    if (Pcm->AmpOn) {
        CsAudioNotify(ctx, CSAudioEndpointTypeSpeaker, CSAudioEndpointStop);
        Pcm->AmpOn = FALSE;
    }
    if (Pcm->ParamsSet && fwHasStream) {
        SendStreamCmd(ctx, Pcm, SOF_IPC_STREAM_PCM_FREE, "PCM_FREE");
    }
    Pcm->ParamsSet = FALSE;
    Pcm->Started = FALSE;
    if (Pcm->Stream) {
        ctx->Bus.CleanupDSP(ctx->Bus.Context, Pcm->Stream);
        ctx->Bus.FreeStream(ctx->Bus.Context, Pcm->Stream);
        Pcm->Stream = NULL;
    }
    if (Pcm->PageTable) {
        MmFreeContiguousMemorySpecifyCache(Pcm->PageTable, PAGE_SIZE, MmNonCached);
        Pcm->PageTable = NULL;
    }
}

ULONG SofPcmPosition(PSOF_PCM Pcm)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (!PcmUsable(ctx, Pcm) || !Pcm->Stream || !Pcm->BufferBytes) {
        return 0;
    }
    ULONG pos = ctx->Bus.StreamPosition(ctx->Bus.Context, Pcm->Stream) % Pcm->BufferBytes;
    if (Pcm->Capture && Pcm->BufVa && Pcm->DmaRunning && Pcm->Channels <= 4) {
        // Track the peak of everything captured since the last call
        ULONG frame = Pcm->Channels * sizeof(SHORT);
        ULONG end = pos - pos % frame;
        ULONG i = Pcm->ScanPos;
        ULONG budget = Pcm->BufferBytes / frame;
        while (i != end && budget--) {
            PSHORT f = (PSHORT)((PUCHAR)Pcm->BufVa + i);
            for (ULONG c = 0; c < Pcm->Channels; c++) {
                LONG v = f[c];
                ULONG a = (ULONG)(v < 0 ? -v : v);
                if (a > Pcm->Peak[c]) Pcm->Peak[c] = a;
            }
            i += frame;
            if (i >= Pcm->BufferBytes) i = 0;
        }
        Pcm->ScanPos = end;
    }
    return pos;
}
