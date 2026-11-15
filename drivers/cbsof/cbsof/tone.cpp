// cbsof milestone 3: load the speaker pipeline and play a test tone.
//
// The pipeline messages come from tools/tplg2ipc.py (spk.cbtp). Stream setup
// follows Linux sof_ipc3_pcm_hw_params / hda_dsp_pcm_hw_params / sof_pcm_trigger
// (BSD-3-Clause). The codec drivers are told about the stream through
// Coolstar's CsAudio callback API, as used by coolstar/max98357a (Apache-2.0).
#include "driver.h"
#include <ntstrsafe.h>

#define CBSOF_TPLG_PATH L"\\SystemRoot\\System32\\drivers\\cbsof\\spk.cbtp"

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

static NTSTATUS LoadTopology(PCBSOF_CONTEXT ctx, UINT32* speakerHost)
{
    PUCHAR blob = NULL;
    ULONG size = 0;
    NTSTATUS status = CbLoadFile(ctx, CBSOF_TPLG_PATH, &blob, &size);
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

    *speakerHost = MAXULONG;
    const CBTP_PCM* pcms = (const CBTP_PCM*)(blob + sizeof(*hdr));
    for (ULONG i = 0; i < hdr->numPcms; i++) {
        if (pcms[i].pcmId == 0 && pcms[i].direction == 0) {
            *speakerHost = pcms[i].hostCompId;
        }
    }

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
        CbLog(ctx, "topology: %u messages accepted, speaker host comp %u", hdr->numMsgs, *speakerHost);
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

// Integer sine table, quarter wave, 256 steps, scaled to 32767.
static INT16 SineQ(ULONG phase /* 0..1023 */)
{
    static const INT16 q[257] = {
        0,201,402,603,804,1005,1206,1406,1607,1808,2009,2209,2410,2610,2811,3011,3211,3411,3611,3811,4011,4210,4409,
        4608,4807,5006,5205,5403,5601,5799,5997,6195,6392,6589,6786,6982,7179,7375,7571,7766,7961,8156,8351,8545,8739,
        8932,9126,9319,9511,9703,9895,10087,10278,10469,10659,10849,11038,11227,11416,11604,11792,11980,12166,12353,
        12539,12724,12909,13094,13278,13462,13645,13827,14009,14191,14372,14552,14732,14911,15090,15268,15446,15623,
        15799,15975,16150,16325,16499,16672,16845,17017,17189,17360,17530,17699,17868,18036,18204,18371,18537,18702,
        18867,19031,19194,19357,19519,19680,19840,20000,20159,20317,20474,20631,20787,20942,21096,21249,21402,21554,
        21705,21855,22004,22153,22301,22448,22594,22739,22883,23027,23169,23311,23452,23592,23731,23869,24006,24143,
        24278,24413,24546,24679,24811,24942,25072,25201,25329,25456,25582,25707,25831,25954,26077,26198,26318,26437,
        26556,26673,26789,26905,27019,27132,27244,27355,27466,27575,27683,27790,27896,28001,28105,28208,28309,28410,
        28510,28608,28706,28802,28897,28992,29085,29177,29268,29358,29446,29534,29621,29706,29790,29873,29955,30036,
        30116,30195,30272,30349,30424,30498,30571,30643,30713,30783,30851,30918,30984,31049,31113,31175,31236,31297,
        31356,31413,31470,31525,31580,31633,31684,31735,31785,31833,31880,31926,31970,32014,32056,32097,32137,32176,
        32213,32249,32284,32318,32350,32382,32412,32441,32468,32495,32520,32544,32567,32588,32609,32628,32646,32662,
        32678,32692,32705,32717,32727,32736,32744,32751,32757,32761,32764,32766,32767 };
    phase &= 1023;
    if (phase < 256) return q[phase];
    if (phase < 512) return q[512 - phase];
    if (phase < 768) return (INT16)-q[phase - 512];
    return (INT16)-q[1024 - phase];
}

// ---------------------------------------------------------------------------
// Speaker tone test
// ---------------------------------------------------------------------------
#define TONE_PERIOD_BYTES   4096                 // 1024 stereo S16 frames
#define TONE_PERIODS        4
#define TONE_BUFFER_BYTES   (TONE_PERIOD_BYTES * TONE_PERIODS)
#define TONE_FRAMES         (TONE_BUFFER_BYTES / 4)
#define TONE_CYCLES         40                   // 40 cycles per buffer = 468.75 Hz
#define TONE_SECONDS        3

NTSTATUS DspSpeakerToneTest(PCBSOF_CONTEXT ctx)
{
    NTSTATUS status;
    UINT32 hostComp = MAXULONG;
    HANDLE stream = NULL;
    UINT8 tag = 0;
    PUCHAR buf = NULL, pageTable = NULL;
    BOOLEAN dmaRunning = FALSE, ipcRunning = FALSE, paramsSet = FALSE, ampOn = FALSE;
    PHYSICAL_ADDRESS low = {}, high = {}, boundary = {}, phys = {}, ptPhys = {};
    PVOID bdlVa = NULL;
    HDAUDIO_STREAM_FORMAT fmt = {};
    sof_ipc_pcm_params pcm;
    sof_ipc_stream trig;

    // Let the codec drivers announce themselves.
    ctx->CodecEndpoints = 0;
    CsAudioNotify(ctx, CSAudioEndpointTypeDSP, CSAudioEndpointRegister);
    CbLog(ctx, "codec endpoints registered: mask 0x%X (bit1 = speaker amp)", ctx->CodecEndpoints);

    status = LoadTopology(ctx, &hostComp);
    if (!NT_SUCCESS(status) || hostComp == MAXULONG) {
        CbLog(ctx, "speaker topology load failed 0x%08X", status);
        return NT_SUCCESS(status) ? STATUS_NOT_FOUND : status;
    }

    // Host DMA stream: 48 kHz, 16-bit, stereo
    fmt.SampleRate = 48000;
    fmt.ValidBitsPerSample = 16;
    fmt.ContainerSize = 16;
    fmt.NumberOfChannels = 2;
    status = ctx->Bus.GetRenderStream(ctx->Bus.Context, fmt, &stream, &tag);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "GetRenderStream failed 0x%08X", status);
        return status;
    }

    high.QuadPart = 0xFFFFFFFF;
    buf = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(TONE_BUFFER_BYTES, low, high, boundary, MmNonCached);
    pageTable = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, low, high, boundary, MmNonCached);
    if (!buf || !pageTable) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    phys = MmGetPhysicalAddress(buf);
    ptPhys = MmGetPhysicalAddress(pageTable);

    // 468.75 Hz sine, -12 dBFS, same on both channels
    {
        INT16* s = (INT16*)buf;
        for (ULONG f = 0; f < TONE_FRAMES; f++) {
            INT16 v = (INT16)(SineQ((ULONG)(((ULONGLONG)f * TONE_CYCLES * 1024) / TONE_FRAMES)) / 4);
            s[2 * f] = v;
            s[2 * f + 1] = v;
        }
    }

    // Compressed 20-bit PFN page table (snd_sof_create_page_table)
    RtlZeroMemory(pageTable, PAGE_SIZE);
    for (ULONG i = 0; i < TONE_BUFFER_BYTES / PAGE_SIZE; i++) {
        ULONG pfn = (ULONG)((phys.QuadPart + (LONGLONG)i * PAGE_SIZE) >> PAGE_SHIFT);
        PUCHAR p = pageTable + ((5 * i) >> 1);
        ULONG v;
        if (i & 1) {
            v = (p[0] & 0xF) | (pfn << 4);
        } else {
            v = pfn;
        }
        p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); p[2] = (UCHAR)(v >> 16); p[3] = (UCHAR)(v >> 24);
    }

    status = ctx->Bus.PrepareDSP(ctx->Bus.Context, stream, TONE_BUFFER_BYTES, TONE_PERIODS, &bdlVa);
    if (!NT_SUCCESS(status) || !bdlVa) {
        CbLog(ctx, "PrepareDSP failed 0x%08X", status);
        if (NT_SUCCESS(status)) status = STATUS_UNSUCCESSFUL;
        goto out;
    }
    {
        PHDA_BDLE bdl = (PHDA_BDLE)bdlVa;
        for (ULONG i = 0; i < TONE_PERIODS; i++) {
            ULONGLONG a = (ULONGLONG)phys.QuadPart + (ULONGLONG)i * TONE_PERIOD_BYTES;
            bdl[i].AddrLo = (ULONG)a;
            bdl[i].AddrHi = (ULONG)(a >> 32);
            bdl[i].Size = TONE_PERIOD_BYTES;
            bdl[i].Ioc = 0;   // positions come from the DPIB buffer, not interrupts
        }
        KeMemoryBarrier();
    }
    ApplyProcenFormatQuirk(ctx, tag, TRUE);

    // STREAM_PCM_PARAMS
    RtlZeroMemory(&pcm, sizeof(pcm));
    pcm.hdr.size = sizeof(pcm);
    pcm.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_PARAMS;
    pcm.comp_id = hostComp;
    pcm.params.hdr.size = sizeof(pcm.params);
    pcm.params.buffer.hdr.size = sizeof(pcm.params.buffer);
    pcm.params.buffer.phy_addr = ptPhys.LowPart;
    pcm.params.buffer.pages = TONE_BUFFER_BYTES / PAGE_SIZE;
    pcm.params.buffer.size = TONE_BUFFER_BYTES;
    pcm.params.direction = 0;
    pcm.params.frame_fmt = 0;            // S16_LE
    pcm.params.buffer_fmt = 0;           // interleaved
    pcm.params.rate = 48000;
    pcm.params.stream_tag = tag;
    pcm.params.channels = 2;
    pcm.params.sample_valid_bytes = 2;
    pcm.params.sample_container_bytes = 2;
    pcm.params.host_period_bytes = TONE_PERIOD_BYTES;
    pcm.params.no_stream_position = 1;   // HDA: positions from DPIB
    status = IpcTxCheck(ctx, &pcm, sizeof(pcm), "PCM_PARAMS");
    if (!NT_SUCCESS(status)) {
        goto out;
    }
    paramsSet = TRUE;
    {
        const sof_ipc_pcm_params_reply* r = (const sof_ipc_pcm_params_reply*)ctx->ReplyBuf;
        CbLog(ctx, "PCM_PARAMS ok: comp %u posn_offset 0x%X", r->comp_id, r->posn_offset);
    }

    // Amp on, then host DMA, then the firmware stream (sof_pcm_trigger order)
    CsAudioNotify(ctx, CSAudioEndpointTypeSpeaker, CSAudioEndpointStart);
    ampOn = TRUE;

    ctx->Bus.TriggerDSP(ctx->Bus.Context, stream, TRUE);
    dmaRunning = TRUE;

    RtlZeroMemory(&trig, sizeof(trig));
    trig.hdr.size = sizeof(trig);
    trig.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_START;
    trig.comp_id = hostComp;
    status = IpcTxCheck(ctx, &trig, sizeof(trig), "TRIG_START");
    if (!NT_SUCCESS(status)) {
        goto out;
    }
    ipcRunning = TRUE;
    CbLog(ctx, "tone playing for %u s", TONE_SECONDS);

    {
        ULONG lastPos = 0, moves = 0;
        for (ULONG i = 0; i < TONE_SECONDS * 4; i++) {
            LARGE_INTEGER t;
            t.QuadPart = -2500000LL;   // 250 ms
            KeDelayExecutionThread(KernelMode, FALSE, &t);
            ULONG pos = ctx->Bus.StreamPosition(ctx->Bus.Context, stream);
            if (pos != lastPos) moves++;
            if (i % 4 == 3) {
                CbLog(ctx, "  t=%u ms DMA position %u", (i + 1) * 250, pos);
            }
            lastPos = pos;
        }
        CbLog(ctx, "DMA position changed in %u of %u samples", moves, TONE_SECONDS * 4);
        if (ctx->PanicCode) {
            CbLog(ctx, "firmware PANIC 0x%08X during playback", ctx->PanicCode);
        }
    }

out:
    if (ipcRunning) {
        RtlZeroMemory(&trig, sizeof(trig));
        trig.hdr.size = sizeof(trig);
        trig.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_STOP;
        trig.comp_id = hostComp;
        IpcTxCheck(ctx, &trig, sizeof(trig), "TRIG_STOP");
    }
    if (dmaRunning) {
        ctx->Bus.TriggerDSP(ctx->Bus.Context, stream, FALSE);
    }
    if (ampOn) {
        CsAudioNotify(ctx, CSAudioEndpointTypeSpeaker, CSAudioEndpointStop);
    }
    if (paramsSet) {
        RtlZeroMemory(&trig, sizeof(trig));
        trig.hdr.size = sizeof(trig);
        trig.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_FREE;
        trig.comp_id = hostComp;
        IpcTxCheck(ctx, &trig, sizeof(trig), "PCM_FREE");
    }
    if (stream) {
        ctx->Bus.CleanupDSP(ctx->Bus.Context, stream);
        ctx->Bus.FreeStream(ctx->Bus.Context, stream);
    }
    if (buf) {
        MmFreeContiguousMemorySpecifyCache(buf, TONE_BUFFER_BYTES, MmNonCached);
    }
    if (pageTable) {
        MmFreeContiguousMemorySpecifyCache(pageTable, PAGE_SIZE, MmNonCached);
    }
    return status;
}
