// cbsof: SOF firmware boot and IPC3 for Intel cAVS 1.5 (Gemini Lake).
//
// The sequence follows the Linux SOF host driver (sound/soc/sof/intel/
// hda-loader.c, hda-dsp.c, hda-ipc.c and sound/soc/sof/ipc3.c), used under
// its BSD-3-Clause license. Copyright(c) 2017-2022 Intel Corporation.
#include "driver.h"

// ---------------------------------------------------------------------------
// Register helpers
// ---------------------------------------------------------------------------
static __forceinline ULONG DspRead(PCBSOF_CONTEXT ctx, ULONG off)
{
    return READ_REGISTER_ULONG((volatile ULONG*)(ctx->DspBar + off));
}

static __forceinline void DspWrite(PCBSOF_CONTEXT ctx, ULONG off, ULONG val)
{
    WRITE_REGISTER_ULONG((volatile ULONG*)(ctx->DspBar + off), val);
}

static void DspUpdate(PCBSOF_CONTEXT ctx, ULONG off, ULONG mask, ULONG val)
{
    ULONG old = DspRead(ctx, off);
    ULONG nv = (old & ~mask) | (val & mask);
    if (nv != old) {
        DspWrite(ctx, off, nv);
    }
}

// Always writes, for write-1-to-clear bits.
static void DspUpdateForced(PCBSOF_CONTEXT ctx, ULONG off, ULONG mask, ULONG val)
{
    ULONG old = DspRead(ctx, off);
    DspWrite(ctx, off, (old & ~mask) | (val & mask));
}

static void PpUpdate(PCBSOF_CONTEXT ctx, ULONG off, ULONG mask, ULONG val)
{
    volatile ULONG* reg = (volatile ULONG*)(ctx->PpCap + off);
    ULONG old = READ_REGISTER_ULONG(reg);
    ULONG nv = (old & ~mask) | (val & mask);
    if (nv != old) {
        WRITE_REGISTER_ULONG(reg, nv);
    }
}

static void SleepUs(ULONG us)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)us * 10;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static ULONGLONG NowUs()
{
    return KeQueryInterruptTime() / 10;
}

// Poll until (reg & mask) == expected. Returns the last value read.
static NTSTATUS PollDsp(PCBSOF_CONTEXT ctx, ULONG off, ULONG mask, ULONG expected,
                        ULONG timeoutUs, ULONG* lastVal)
{
    ULONGLONG end = NowUs() + timeoutUs;
    ULONG v;
    for (;;) {
        v = DspRead(ctx, off);
        if ((v & mask) == expected) {
            break;
        }
        if (NowUs() > end) {
            if (lastVal) *lastVal = v;
            return STATUS_IO_TIMEOUT;
        }
        SleepUs(500);
    }
    if (lastVal) *lastVal = v;
    return STATUS_SUCCESS;
}

static NTSTATUS PollRomState(PCBSOF_CONTEXT ctx, ULONG state, ULONG timeoutUs, ULONG* lastVal)
{
    ULONGLONG end = NowUs() + timeoutUs;
    ULONG v;
    for (;;) {
        v = DspRead(ctx, DSP_SRAM_ROM_STATUS);
        if (FSR_TO_STATE(v) == state) {
            break;
        }
        if (NowUs() > end) {
            if (lastVal) *lastVal = v;
            return STATUS_IO_TIMEOUT;
        }
        SleepUs(500);
    }
    if (lastVal) *lastVal = v;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// DSP core control (hda-dsp.c)
// ---------------------------------------------------------------------------
static BOOLEAN CoreIsEnabled(PCBSOF_CONTEXT ctx, ULONG cm)
{
    ULONG v = DspRead(ctx, DSP_REG_ADSPCS);
    return ((v & ADSPCS_SPA(cm)) == ADSPCS_SPA(cm)) &&
           ((v & ADSPCS_CPA(cm)) == ADSPCS_CPA(cm)) &&
           ((v & ADSPCS_CRST(cm)) == 0) &&
           ((v & ADSPCS_CSTALL(cm)) == 0);
}

static NTSTATUS CoreResetEnter(PCBSOF_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_CRST(cm), ADSPCS_CRST(cm));
    return PollDsp(ctx, DSP_REG_ADSPCS, ADSPCS_CRST(cm), ADSPCS_CRST(cm), DSP_RESET_TIMEOUT_US, NULL);
}

static NTSTATUS CoreResetLeave(PCBSOF_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_CRST(cm), 0);
    return PollDsp(ctx, DSP_REG_ADSPCS, ADSPCS_CRST(cm), 0, DSP_RESET_TIMEOUT_US, NULL);
}

static NTSTATUS CoreStallReset(PCBSOF_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_CSTALL(cm), ADSPCS_CSTALL(cm));
    return CoreResetEnter(ctx, cm);
}

static NTSTATUS CorePowerUp(PCBSOF_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_SPA(cm), ADSPCS_SPA(cm));
    return PollDsp(ctx, DSP_REG_ADSPCS, ADSPCS_CPA(cm), ADSPCS_CPA(cm), DSP_RESET_TIMEOUT_US, NULL);
}

static NTSTATUS CorePowerDown(PCBSOF_CONTEXT ctx, ULONG cm)
{
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_SPA(cm), 0);
    return PollDsp(ctx, DSP_REG_ADSPCS, ADSPCS_CPA(cm), 0, DSP_PD_TIMEOUT_US, NULL);
}

static NTSTATUS CoreRun(PCBSOF_CONTEXT ctx, ULONG cm)
{
    NTSTATUS status = CoreResetLeave(ctx, cm);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    DspUpdate(ctx, DSP_REG_ADSPCS, ADSPCS_CSTALL(cm), 0);
    if (!CoreIsEnabled(ctx, cm)) {
        CoreStallReset(ctx, cm);
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS CoreResetPowerDown(PCBSOF_CONTEXT ctx, ULONG cm)
{
    NTSTATUS status = CoreStallReset(ctx, cm);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return CorePowerDown(ctx, cm);
}

static void IpcIntEnable(PCBSOF_CONTEXT ctx, BOOLEAN enable)
{
    ULONG v = enable ? (HIPCCTL_DONE | HIPCCTL_BUSY) : 0;
    DspUpdate(ctx, DSP_REG_HIPCCTL, HIPCCTL_DONE | HIPCCTL_BUSY, v);
    DspUpdate(ctx, DSP_REG_ADSPIC, ADSPIC_IPC, enable ? ADSPIC_IPC : 0);
}

// ---------------------------------------------------------------------------
// Firmware file
// ---------------------------------------------------------------------------
static NTSTATUS LoadFirmwareFile(PCBSOF_CONTEXT ctx, PUCHAR* data, ULONG* size)
{
    UNICODE_STRING path;
    RtlInitUnicodeString(&path, CBSOF_FW_PATH);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    HANDLE file;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = ZwCreateFile(&file, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL,
                                   FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                                   FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "cannot open %ws (0x%08X)", CBSOF_FW_PATH, status);
        return status;
    }

    FILE_STANDARD_INFORMATION info;
    status = ZwQueryInformationFile(file, &iosb, &info, sizeof(info), FileStandardInformation);
    if (!NT_SUCCESS(status) || info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > 4 * 1024 * 1024) {
        ZwClose(file);
        CbLog(ctx, "bad firmware file size");
        return NT_SUCCESS(status) ? STATUS_INVALID_IMAGE_FORMAT : status;
    }

    ULONG len = (ULONG)info.EndOfFile.QuadPart;
    PUCHAR buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, len, CBSOF_POOL_TAG);
    if (!buf) {
        ZwClose(file);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    LARGE_INTEGER offset = {};
    status = ZwReadFile(file, NULL, NULL, NULL, &iosb, buf, len, &offset, NULL);
    ZwClose(file);
    if (!NT_SUCCESS(status) || iosb.Information != len) {
        ExFreePoolWithTag(buf, CBSOF_POOL_TAG);
        CbLog(ctx, "firmware read failed 0x%08X", status);
        return NT_SUCCESS(status) ? STATUS_END_OF_FILE : status;
    }
    *data = buf;
    *size = len;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Code loader init (cl_dsp_init in hda-loader.c)
// ---------------------------------------------------------------------------
static NTSTATUS ClDspInit(PCBSOF_CONTEXT ctx, UINT8 streamTag)
{
    ULONG v = 0;

    // step 1: power up cores 0 and 1
    NTSTATUS status = CorePowerUp(ctx, GLK_CORE_MASK_ALL);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "  core power up failed, ADSPCS=0x%08X", DspRead(ctx, DSP_REG_ADSPCS));
        goto err;
    }

    // step 2: ROM_CONTROL + purge, with the code loader stream tag
    DspWrite(ctx, DSP_REG_HIPCI, HIPCI_BUSY | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW |
                                 ((ULONG)(streamTag - 1) << 9));

    // step 3: take core 0 out of reset and run it
    status = CoreRun(ctx, GLK_CORE_MASK_INIT);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "  core 0 run failed, ADSPCS=0x%08X", DspRead(ctx, DSP_REG_ADSPCS));
        goto err;
    }

    // step 4: wait for the ROM to acknowledge the IPC
    status = PollDsp(ctx, DSP_REG_HIPCIE, HIPCIE_DONE, HIPCIE_DONE, DSP_INIT_TIMEOUT_US, &v);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "  timeout waiting for HIPCIE done (0x%08X), ROM status 0x%08X",
              v, DspRead(ctx, DSP_SRAM_ROM_STATUS));
        goto err;
    }
    DspUpdateForced(ctx, DSP_REG_HIPCIE, HIPCIE_DONE, HIPCIE_DONE);

    // step 5: core 1 is not needed for boot
    status = CoreResetPowerDown(ctx, GLK_CORE_MASK_ALL & ~GLK_CORE_MASK_INIT);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "  core 1 power down failed");
        goto err;
    }

    // step 6: enable IPC interrupts
    IpcIntEnable(ctx, TRUE);

    // step 7: wait for ROM init done
    status = PollRomState(ctx, FSR_STATE_INIT_DONE, DSP_ROM_INIT_TIMEOUT_US, &v);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "  timeout waiting for ROM init, status 0x%08X error 0x%08X",
              v, DspRead(ctx, DSP_SRAM_ROM_ERROR));
        goto err;
    }
    return STATUS_SUCCESS;

err:
    IpcIntEnable(ctx, FALSE);
    CoreResetPowerDown(ctx, GLK_CORE_MASK_ALL);
    return status;
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
NTSTATUS DspBoot(PCBSOF_CONTEXT ctx)
{
    NTSTATUS status;
    PUCHAR fw = NULL;
    ULONG fwSize = 0;
    PUCHAR dmaBuf = NULL;
    HANDLE stream = NULL;
    UINT8 tag = 0;
    ULONG v = 0;
    ULONG payloadOff = 0;
    ULONG payloadSize = 0;
    const sof_ext_man_header* xman = NULL;
    HDAUDIO_STREAM_FORMAT fmt = {};
    PHYSICAL_ADDRESS low = {}, high = {}, boundary = {}, phys = {};
    PVOID bdlVa = NULL;

    ctx->FwReady = FALSE;
    ctx->PanicCode = 0;
    KeClearEvent(&ctx->FwReadyEvent);
    KeClearEvent(&ctx->ReplyEvent);

    CbLog(ctx, "boot: ADSPCS=0x%08X ROM status=0x%08X PPCTL=0x%08X",
          DspRead(ctx, DSP_REG_ADSPCS), DspRead(ctx, DSP_SRAM_ROM_STATUS),
          READ_REGISTER_ULONG((volatile ULONG*)(ctx->PpCap + PP_PPCTL)));

    // Enable the DSP and its interrupt in the processing pipe capability.
    PpUpdate(ctx, PP_PPCTL, PPCTL_GPROCEN, PPCTL_GPROCEN);
    PpUpdate(ctx, PP_PPCTL, PPCTL_PIE, PPCTL_PIE);

    // Start from a known state.
    IpcIntEnable(ctx, FALSE);
    CoreResetPowerDown(ctx, GLK_CORE_MASK_ALL);

    status = LoadFirmwareFile(ctx, &fw, &fwSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Skip the SOF extended manifest; the ROM wants the image after it.
    xman = (const sof_ext_man_header*)fw;
    if (fwSize > sizeof(*xman) && xman->magic == SOF_EXT_MAN_MAGIC_NUMBER) {
        payloadOff = xman->full_size;
    }
    if (payloadOff >= fwSize) {
        CbLog(ctx, "bad extended manifest size 0x%X", payloadOff);
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto out;
    }
    payloadSize = fwSize - payloadOff;
    CbLog(ctx, "firmware %u bytes, ext manifest 0x%X, payload %u bytes", fwSize, payloadOff, payloadSize);

    // Code loader stream: 48 kHz, 32-bit, mono (HDA_CL_STREAM_FORMAT 0x40).
    fmt.SampleRate = 48000;
    fmt.ValidBitsPerSample = 32;
    fmt.ContainerSize = 32;
    fmt.NumberOfChannels = 1;
    status = ctx->Bus.GetRenderStream(ctx->Bus.Context, fmt, &stream, &tag);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "GetRenderStream failed 0x%08X", status);
        goto out;
    }
    CbLog(ctx, "code loader stream tag %u", tag);

    high.QuadPart = 0xFFFFFFFF;
    dmaBuf = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(payloadSize, low, high, boundary, MmNonCached);
    if (!dmaBuf) {
        CbLog(ctx, "cannot allocate %u bytes of DMA memory", payloadSize);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    RtlCopyMemory(dmaBuf, fw + payloadOff, payloadSize);
    phys = MmGetPhysicalAddress(dmaBuf);

    status = ctx->Bus.PrepareDSP(ctx->Bus.Context, stream, payloadSize, 2, &bdlVa);
    if (!NT_SUCCESS(status) || !bdlVa) {
        CbLog(ctx, "PrepareDSP failed 0x%08X", status);
        if (NT_SUCCESS(status)) status = STATUS_UNSUCCESSFUL;
        goto out;
    }

    // The HDA spec needs at least two BDL entries. Split the buffer in two,
    // keeping the second half 128-byte aligned.
    {
        PHDA_BDLE bdl = (PHDA_BDLE)bdlVa;
        ULONG first = (payloadSize / 2) & ~127u;
        bdl[0].AddrLo = phys.LowPart;
        bdl[0].AddrHi = (ULONG)phys.HighPart;
        bdl[0].Size = first;
        bdl[0].Ioc = 0;
        ULONGLONG p2 = (ULONGLONG)phys.QuadPart + first;
        bdl[1].AddrLo = (ULONG)p2;
        bdl[1].AddrHi = (ULONG)(p2 >> 32);
        bdl[1].Size = payloadSize - first;
        bdl[1].Ioc = 1;
        KeMemoryBarrier();
    }
    ctx->Bus.DSPEnableSPIB(ctx->Bus.Context, stream, payloadSize);

    for (int i = 1; i <= DSP_FW_BOOT_ATTEMPTS; i++) {
        status = ClDspInit(ctx, tag);
        CbLog(ctx, "ROM init attempt %d: 0x%08X", i, status);
        if (NT_SUCCESS(status)) {
            break;
        }
    }

    if (NT_SUCCESS(status)) {
        // Run the code loader DMA and wait for the firmware to start.
        ctx->Bus.TriggerDSP(ctx->Bus.Context, stream, TRUE);
        status = PollRomState(ctx, FSR_STATE_FW_ENTERED, DSP_BASEFW_TIMEOUT_US, &v);
        ctx->Bus.TriggerDSP(ctx->Bus.Context, stream, FALSE);
        if (NT_SUCCESS(status)) {
            CbLog(ctx, "firmware entered (status 0x%08X)", v);
        } else {
            CbLog(ctx, "timeout waiting for FW_ENTERED: status 0x%08X error 0x%08X DMA pos %u",
                  v, DspRead(ctx, DSP_SRAM_ROM_ERROR),
                  ctx->Bus.StreamPosition(ctx->Bus.Context, stream));
        }
    }

    ctx->Bus.DSPDisableSPIB(ctx->Bus.Context, stream);
    ctx->Bus.CleanupDSP(ctx->Bus.Context, stream);

    if (NT_SUCCESS(status)) {
        // Wait for SOF_IPC_FW_READY on the DSP->host doorbell.
        LARGE_INTEGER timeout;
        timeout.QuadPart = -20000000LL; // 2 s
        NTSTATUS wait = KeWaitForSingleObject(&ctx->FwReadyEvent, Executive, KernelMode, FALSE, &timeout);
        if (wait == STATUS_TIMEOUT) {
            CbLog(ctx, "no FW_READY after 2 s: HIPCT=0x%08X HIPCTE=0x%08X ADSPIS=0x%08X HIPCCTL=0x%08X irqs=%d ROM status 0x%08X",
                  DspRead(ctx, DSP_REG_HIPCT), DspRead(ctx, DSP_REG_HIPCTE), DspRead(ctx, DSP_REG_ADSPIS),
                  DspRead(ctx, DSP_REG_HIPCCTL), ctx->IrqCount, DspRead(ctx, DSP_SRAM_ROM_STATUS));
            status = STATUS_IO_TIMEOUT;
        } else if (ctx->PanicCode) {
            CbLog(ctx, "firmware PANIC 0x%08X", ctx->PanicCode);
            status = STATUS_DEVICE_HARDWARE_ERROR;
        } else {
            const sof_ipc_fw_version* ver = &ctx->FwReadyMsg.version;
            CbLog(ctx, "FW_READY: SOF %u.%u.%u build %u tag %.6s ABI 0x%08X date %.12s %.10s",
                  ver->major, ver->minor, ver->micro, ver->build, (const char*)ver->tag,
                  ver->abi_version, (const char*)ver->date, (const char*)ver->time);
            CbLog(ctx, "mailboxes: host->dsp 0x%X/%u, dsp->host 0x%X/%u, stream 0x%X/%u, irqs=%d",
                  ctx->HostBoxOffset, ctx->HostBoxSize, ctx->DspBoxOffset, ctx->DspBoxSize,
                  ctx->StreamBoxOffset, ctx->StreamBoxSize, ctx->IrqCount);
        }
    }

out:
    if (stream) {
        ctx->Bus.FreeStream(ctx->Bus.Context, stream);
    }
    if (dmaBuf) {
        MmFreeContiguousMemorySpecifyCache(dmaBuf, payloadSize, MmNonCached);
    }
    if (fw) {
        ExFreePoolWithTag(fw, CBSOF_POOL_TAG);
    }
    if (!NT_SUCCESS(status)) {
        DspShutdown(ctx);
    }
    return status;
}

void DspShutdown(PCBSOF_CONTEXT ctx)
{
    if (!ctx->DspBar || !ctx->PpCap) {
        return;
    }
    IpcIntEnable(ctx, FALSE);
    CoreResetPowerDown(ctx, GLK_CORE_MASK_ALL);
    PpUpdate(ctx, PP_PPCTL, PPCTL_PIE, 0);
    PpUpdate(ctx, PP_PPCTL, PPCTL_GPROCEN, 0);
    KeFlushQueuedDpcs();
    ctx->FwReady = FALSE;
}

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------
static BOOLEAN InBar(PCBSOF_CONTEXT ctx, ULONG off, ULONG len)
{
    return off < ctx->DspBarLen && len <= ctx->DspBarLen - off;
}

static void MboxRead(PCBSOF_CONTEXT ctx, ULONG off, void* dst, ULONG len)
{
    if (!InBar(ctx, off, len)) {
        RtlZeroMemory(dst, len);
        return;
    }
    READ_REGISTER_BUFFER_UCHAR(ctx->DspBar + off, (PUCHAR)dst, len);
}

static void MboxWrite(PCBSOF_CONTEXT ctx, ULONG off, const void* src, ULONG len)
{
    if (!InBar(ctx, off, len)) {
        return;
    }
    WRITE_REGISTER_BUFFER_UCHAR(ctx->DspBar + off, (PUCHAR)src, len);
}

// Called by sklhdaudbus from its ISR, at DIRQL. Latch and mask only.
BOOL DspInterruptCallback(PVOID context)
{
    PCBSOF_CONTEXT ctx = (PCBSOF_CONTEXT)context;
    if (!ctx || !ctx->DspBar) {
        return FALSE;
    }
    ULONG is = DspRead(ctx, DSP_REG_ADSPIS);
    if (is == 0xFFFFFFFF || !(is & ADSPIS_IPC)) {
        return FALSE;
    }

    BOOLEAN queued = FALSE;
    ULONG hipcie = DspRead(ctx, DSP_REG_HIPCIE);
    ULONG hipct = DspRead(ctx, DSP_REG_HIPCT);

    if (hipcie & HIPCIE_DONE) {
        ULONG hipci = DspRead(ctx, DSP_REG_HIPCI);
        DspUpdate(ctx, DSP_REG_HIPCCTL, HIPCCTL_DONE, 0);
        InterlockedExchange(&ctx->ReplyMsg, (LONG)(hipci & HIPCI_MSG_MASK));
        InterlockedExchange(&ctx->ReplyLatched, 1);
        queued = TRUE;
    }
    if (hipct & HIPCT_BUSY) {
        ULONG hipcte = DspRead(ctx, DSP_REG_HIPCTE);
        DspUpdate(ctx, DSP_REG_HIPCCTL, HIPCCTL_BUSY, 0);
        InterlockedExchange(&ctx->HipctMsg, (LONG)(hipct & HIPCT_MSG_MASK));
        InterlockedExchange(&ctx->HipcteMsg, (LONG)(hipcte & HIPCTE_MSG_MASK));
        InterlockedExchange(&ctx->HipctLatched, 1);
        queued = TRUE;
    }
    InterlockedIncrement(&ctx->IrqCount);
    if (queued) {
        KeInsertQueueDpc(&ctx->IpcDpc, NULL, NULL);
    }
    return TRUE;
}

static void ParseFwReady(PCBSOF_CONTEXT ctx)
{
    MboxRead(ctx, DSP_MBOX_UPLINK_OFFSET, &ctx->FwReadyMsg, sizeof(ctx->FwReadyMsg));

    // Extended data follows fw_ready: a chain of sof_ipc_ext_data_hdr, each
    // with hdr.cmd == SOF_IPC_FW_READY.
    ULONG off = DSP_MBOX_UPLINK_OFFSET + sizeof(sof_ipc_fw_ready);
    for (int n = 0; n < 16; n++) {
        sof_ipc_ext_data_hdr ext;
        MboxRead(ctx, off, &ext, sizeof(ext));
        if (ext.hdr.cmd != SOF_IPC_FW_READY || ext.hdr.size < sizeof(ext) || ext.hdr.size > 4096) {
            break;
        }
        if (ext.type == SOF_IPC_EXT_WINDOW) {
            sof_ipc_window win;
            RtlZeroMemory(&win, sizeof(win));
            MboxRead(ctx, off, &win, min(ext.hdr.size, (UINT32)sizeof(win)));
            ULONG count = min(win.num_windows, (UINT32)SOF_IPC_MAX_ELEMS);
            for (ULONG i = 0; i < count; i++) {
                const sof_ipc_window_elem* e = &win.window[i];
                ULONG woff = SRAM_WINDOW_OFFSET(e->id) + e->offset;
                switch (e->type) {
                case SOF_IPC_REGION_UPBOX:
                    ctx->DspBoxOffset = woff;
                    ctx->DspBoxSize = e->size;
                    break;
                case SOF_IPC_REGION_DOWNBOX:
                    ctx->HostBoxOffset = woff;
                    ctx->HostBoxSize = e->size;
                    break;
                case SOF_IPC_REGION_STREAM:
                    ctx->StreamBoxOffset = woff;
                    ctx->StreamBoxSize = e->size;
                    break;
                default:
                    break;
                }
            }
        }
        off += ext.hdr.size;
    }
}

void DspIpcDpc(PKDPC Dpc, PVOID DeferredContext, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    PCBSOF_CONTEXT ctx = (PCBSOF_CONTEXT)DeferredContext;

    // Reply to a host-initiated message.
    if (InterlockedExchange(&ctx->ReplyLatched, 0)) {
        if (ctx->FwReady && ctx->HostBoxSize) {
            sof_ipc_cmd_hdr hdr;
            MboxRead(ctx, ctx->HostBoxOffset, &hdr, sizeof(hdr));
            ULONG len = min(hdr.size, (UINT32)SOF_IPC_MSG_MAX_SIZE);
            if (len < sizeof(hdr)) len = sizeof(hdr);
            MboxRead(ctx, ctx->HostBoxOffset, ctx->ReplyBuf, len);
        }
        // Tell the DSP we consumed the reply, then unmask DONE.
        DspUpdateForced(ctx, DSP_REG_HIPCIE, HIPCIE_DONE, HIPCIE_DONE);
        DspUpdate(ctx, DSP_REG_HIPCCTL, HIPCCTL_DONE, HIPCCTL_DONE);
        KeSetEvent(&ctx->ReplyEvent, IO_NO_INCREMENT, FALSE);
    }

    // DSP-initiated message.
    if (InterlockedExchange(&ctx->HipctLatched, 0)) {
        ULONG msg = (ULONG)ctx->HipctMsg;
        if ((msg & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
            InterlockedExchange(&ctx->PanicCode, (LONG)msg);
            KeSetEvent(&ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
        } else {
            ULONG box = ctx->FwReady ? ctx->DspBoxOffset : DSP_MBOX_UPLINK_OFFSET;
            sof_ipc_cmd_hdr hdr;
            MboxRead(ctx, box, &hdr, sizeof(hdr));
            if ((hdr.cmd & SOF_GLB_TYPE_MASK) == SOF_IPC_FW_READY && !ctx->FwReady) {
                ParseFwReady(ctx);
                ctx->FwReady = (ctx->HostBoxSize != 0 && ctx->DspBoxSize != 0);
                KeSetEvent(&ctx->FwReadyEvent, IO_NO_INCREMENT, FALSE);
            } else {
                DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL,
                           "cbsof: DSP message cmd 0x%08X size %u\n", hdr.cmd, hdr.size);
            }
        }
        // Clear BUSY (write 1) to acknowledge, then unmask.
        DspUpdateForced(ctx, DSP_REG_HIPCT, HIPCT_BUSY, HIPCT_BUSY);
        DspUpdate(ctx, DSP_REG_HIPCCTL, HIPCCTL_BUSY, HIPCCTL_BUSY);
    }
}

// Send one IPC3 message and wait for the reply (passive level).
static NTSTATUS IpcTx(PCBSOF_CONTEXT ctx, const void* msg, ULONG len, ULONG timeoutMs)
{
    if (!ctx->FwReady || len > ctx->HostBoxSize || len > SOF_IPC_MSG_MAX_SIZE) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (DspRead(ctx, DSP_REG_HIPCI) & HIPCI_BUSY) {
        return STATUS_DEVICE_BUSY;
    }
    KeClearEvent(&ctx->ReplyEvent);
    RtlZeroMemory(ctx->ReplyBuf, sizeof(ctx->ReplyBuf));
    MboxWrite(ctx, ctx->HostBoxOffset, msg, len);
    DspWrite(ctx, DSP_REG_HIPCI, HIPCI_BUSY);

    LARGE_INTEGER timeout;
    timeout.QuadPart = -(LONGLONG)timeoutMs * 10000;
    NTSTATUS wait = KeWaitForSingleObject(&ctx->ReplyEvent, Executive, KernelMode, FALSE, &timeout);
    if (wait == STATUS_TIMEOUT) {
        CbLog(ctx, "IPC 0x%08X: no reply (HIPCI=0x%08X HIPCIE=0x%08X)",
              ((const sof_ipc_cmd_hdr*)msg)->cmd, DspRead(ctx, DSP_REG_HIPCI), DspRead(ctx, DSP_REG_HIPCIE));
        return STATUS_IO_TIMEOUT;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DspIpcTest(PCBSOF_CONTEXT ctx)
{
    sof_ipc_pm_ctx pm;
    RtlZeroMemory(&pm, sizeof(pm));
    pm.hdr.size = sizeof(pm);
    pm.hdr.cmd = SOF_IPC_GLB_PM_MSG | SOF_IPC_PM_CTX_SIZE;

    NTSTATUS status = IpcTx(ctx, &pm, sizeof(pm), 500);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const sof_ipc_reply* reply = (const sof_ipc_reply*)ctx->ReplyBuf;
    CbLog(ctx, "PM_CTX_SIZE reply: cmd 0x%08X size %u error %d",
          reply->hdr.cmd, reply->hdr.size, reply->error);
    // Any well-formed reply proves the round trip, even an error code.
    return (reply->hdr.cmd & SOF_GLB_TYPE_MASK) == SOF_IPC_GLB_REPLY ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}
