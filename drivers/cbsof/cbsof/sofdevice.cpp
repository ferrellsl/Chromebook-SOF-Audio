// cbsof: DSP device lifetime, owned by the PortCls adapter (CAdapterCommon).
#include "driver.h"
#include <ntstrsafe.h>
#include <stdarg.h>

extern "C" NTSTATUS PcGetPhysicalDeviceObject(PDEVICE_OBJECT DeviceObject, PDEVICE_OBJECT* PhysicalDeviceObject);

PCBSOF_CONTEXT g_Cbsof = NULL;
static volatile LONG g_InstanceCounter = 0;

// ---------------------------------------------------------------------------
// Logging: DbgPrint plus an in-memory log written to the device's hardware
// key as "CbsofLog" (REG_SZ).
// ---------------------------------------------------------------------------
void CbLog(PCBSOF_CONTEXT ctx, const char* fmt, ...)
{
    CHAR line[256];
    // "[  41.237] " = seconds since boot, to line up with other events
    ULONGLONG ms = KeQueryInterruptTime() / 10000;
    RtlStringCbPrintfA(line, sizeof(line), "[%4llu.%03llu] ", ms / 1000, ms % 1000);
    size_t prefix = 0;
    RtlStringCbLengthA(line, sizeof(line), &prefix);
    va_list args;
    va_start(args, fmt);
    NTSTATUS status = RtlStringCbVPrintfA(line + prefix, sizeof(line) - prefix, fmt, args);
    va_end(args);
    if (!NT_SUCCESS(status) && status != STATUS_BUFFER_OVERFLOW) {
        return;
    }
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "cbsof: %s\n", line);
    if (!ctx) {
        return;
    }

    size_t len = 0;
    RtlStringCbLengthA(line, sizeof(line), &len);
    KIRQL irql;
    KeAcquireSpinLock(&ctx->LogLock, &irql);
    if (ctx->LogLen + len + 3 >= CBSOF_LOG_SIZE) {
        // Keep the newest lines: drop the first half of the log.
        ULONG half = ctx->LogLen / 2;
        while (half < ctx->LogLen && ctx->Log[half] != '\n') half++;
        if (half < ctx->LogLen) half++;
        RtlMoveMemory(ctx->Log, ctx->Log + half, ctx->LogLen - half);
        ctx->LogLen -= half;
    }
    if (ctx->LogLen + len + 3 < CBSOF_LOG_SIZE) {
        RtlCopyMemory(ctx->Log + ctx->LogLen, line, len);
        ctx->LogLen += (ULONG)len;
        ctx->Log[ctx->LogLen++] = '\r';
        ctx->Log[ctx->LogLen++] = '\n';
        ctx->Log[ctx->LogLen] = 0;
    }
    KeReleaseSpinLock(&ctx->LogLock, irql);

    // Save after every line so the log survives a crash.
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        CbFlushLog(ctx);
    }
}

// A one-line status value next to the log, e.g. "CbsofJack" (never scrolls out)
void CbSetStatus(PCBSOF_CONTEXT ctx, PCWSTR valueName, const char* text)
{
    if (!ctx || !ctx->Pdo || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    HANDLE key;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(ctx->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key))) {
        return;
    }
    ANSI_STRING ansi;
    RtlInitAnsiString(&ansi, text);
    UNICODE_STRING value;
    if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&value, &ansi, TRUE))) {
        UNICODE_STRING name;
        RtlInitUnicodeString(&name, valueName);
        ZwSetValueKey(key, &name, 0, REG_SZ, value.Buffer, value.Length + sizeof(WCHAR));
        RtlFreeUnicodeString(&value);
    }
    ZwClose(key);
}

void CbFlushLog(PCBSOF_CONTEXT ctx)
{
    if (!ctx || !ctx->Pdo || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    HANDLE key;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(ctx->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key))) {
        return;
    }

    // Snapshot the log under the lock, convert outside it.
    PCHAR copy = (PCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, CBSOF_LOG_SIZE, CBSOF_POOL_TAG);
    if (copy) {
        KIRQL irql;
        KeAcquireSpinLock(&ctx->LogLock, &irql);
        RtlCopyMemory(copy, ctx->Log, ctx->LogLen + 1);
        KeReleaseSpinLock(&ctx->LogLock, irql);

        ANSI_STRING ansi;
        RtlInitAnsiString(&ansi, copy);
        UNICODE_STRING value;
        if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&value, &ansi, TRUE))) {
            UNICODE_STRING name;
            RtlInitUnicodeString(&name, L"CbsofLog");
            ZwSetValueKey(key, &name, 0, REG_SZ, value.Buffer, value.Length + sizeof(WCHAR));
            RtlFreeUnicodeString(&value);
        }
        ExFreePoolWithTag(copy, CBSOF_POOL_TAG);
    }
    ZwClose(key);
}

// Keep the previous boot's log (it may describe a crash): CbsofLog -> CbsofLogPrevious
static void RotateLog(PCBSOF_CONTEXT ctx)
{
    HANDLE key;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(ctx->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ | KEY_WRITE, &key))) {
        return;
    }
    UNICODE_STRING cur, prev;
    RtlInitUnicodeString(&cur, L"CbsofLog");
    RtlInitUnicodeString(&prev, L"CbsofLogPrevious");
    ULONG len = 0;
    NTSTATUS status = ZwQueryValueKey(key, &cur, KeyValuePartialInformation, NULL, 0, &len);
    if ((status == STATUS_BUFFER_TOO_SMALL || status == STATUS_BUFFER_OVERFLOW) && len > 0 && len < 64 * 1024) {
        PKEY_VALUE_PARTIAL_INFORMATION info =
            (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, len, CBSOF_POOL_TAG);
        if (info) {
            if (NT_SUCCESS(ZwQueryValueKey(key, &cur, KeyValuePartialInformation, info, len, &len)) &&
                info->Type == REG_SZ) {
                ZwSetValueKey(key, &prev, 0, REG_SZ, info->Data, info->DataLength);
            }
            ExFreePoolWithTag(info, CBSOF_POOL_TAG);
        }
    }
    ZwClose(key);
}

// ---------------------------------------------------------------------------
// IRP_MN_QUERY_INTERFACE to the bus PDO for GUID_ADSP_BUS_INTERFACE
// ---------------------------------------------------------------------------
static NTSTATUS QueryAdspInterface(PDEVICE_OBJECT pdo, PADSP_BUS_INTERFACE iface)
{
    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IO_STATUS_BLOCK iosb = {};
    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, pdo, NULL, 0, NULL, &event, &iosb);
    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    PIO_STACK_LOCATION sp = IoGetNextIrpStackLocation(irp);
    sp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    sp->Parameters.QueryInterface.InterfaceType = &GUID_ADSP_BUS_INTERFACE;
    sp->Parameters.QueryInterface.Size = sizeof(ADSP_BUS_INTERFACE);
    sp->Parameters.QueryInterface.Version = 1;
    sp->Parameters.QueryInterface.Interface = (PINTERFACE)iface;
    sp->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    NTSTATUS status = IoCallDriver(pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

// ---------------------------------------------------------------------------
// The bus driver maps its BARs in PrepareHardware and unmaps them in
// ReleaseHardware, and its re-init clears the DSP interrupt callback. Never
// trust cached pointers across power transitions: fetch them again and
// re-register on every power-up. (0.4 crashed by reading a stale BAR4.)
// ---------------------------------------------------------------------------
// Walk the HDA linked capability list in the CURRENT BAR0 mapping to find
// PPCAP. sklhdaudbus fills its own ppcap pointer only when its walk succeeds
// in PrepareHardware, so after a bus restart it can still point into the old,
// unmapped BAR (0.4.2 crashed on that).
static PUCHAR FindPpCap(PCBSOF_CONTEXT ctx, PUCHAR hdaBar, ULONG hdaLen)
{
    if (!hdaBar || hdaLen < 0x100) {
        return NULL;
    }
    ULONG offset = READ_REGISTER_USHORT((volatile USHORT*)(hdaBar + HDA_REG_LLCH));
    for (int n = 0; n < HDA_MAX_CAPS; n++) {
        if (offset == 0 || offset + 8 > hdaLen || (offset & 3)) {
            break;
        }
        ULONG cap = READ_REGISTER_ULONG((volatile ULONG*)(hdaBar + offset));
        if (cap == 0xFFFFFFFF) {
            CbLog(ctx, "HDA capability read returned 0xFFFFFFFF at 0x%X (controller not ready?)", offset);
            break;
        }
        if (HDA_CAP_HDR_ID(cap) == HDA_PP_CAP_ID) {
            return hdaBar + offset;
        }
        ULONG next = HDA_CAP_HDR_NEXT(cap);
        if (next == offset) {
            break;
        }
        offset = next;
    }
    return NULL;
}

static NTSTATUS RefreshBusResources(PCBSOF_CONTEXT ctx)
{
    CB_PCI_BAR hdaBar = {}, dspBar = {};
    PVOID ppcap = NULL;
    NHLT_INFO nhlt = {};
    BUS_INTERFACE_STANDARD pci = {};
    NTSTATUS status = ctx->Bus.GetResources(ctx->Bus.Context, &hdaBar, &dspBar, &ppcap, &nhlt, &pci);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "GetResources failed 0x%08X", status);
        return status;
    }
    if (!hdaBar.Base || !dspBar.Base || dspBar.Len < DSP_MBOX_UPLINK_OFFSET + 0x1000) {
        CbLog(ctx, "HDA or DSP BAR missing (HDA %p len 0x%X, DSP %p len 0x%X)",
              hdaBar.Base, hdaBar.Len, dspBar.Base, dspBar.Len);
        ctx->DspBar = NULL;
        ctx->HdaBar = NULL;
        ctx->PpCap = NULL;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    // 0.4.3 crashed reading the HDA BAR that GetResources had just returned:
    // the bus handed out an unmapped pointer. Check every page we are about
    // to touch before touching it, and refuse instead of crashing.
    {
        PUCHAR h = (PUCHAR)hdaBar.Base;
        PUCHAR d = (PUCHAR)dspBar.Base;
        BOOLEAN hOk = MmIsAddressValid(h) && MmIsAddressValid(h + hdaBar.Len - 4);
        BOOLEAN dOk = MmIsAddressValid(d) &&
                      MmIsAddressValid(d + DSP_MBOX_OFFSET) &&
                      MmIsAddressValid(d + DSP_MBOX_UPLINK_OFFSET) &&
                      MmIsAddressValid(d + dspBar.Len - 4);
        BOOLEAN pOk = ppcap == NULL || MmIsAddressValid(ppcap);
        if (!hOk || !dOk || !pOk) {
            CbLog(ctx, "bus returned UNMAPPED resources: HDA %p len 0x%X %s, DSP %p len 0x%X %s, PPCAP %p %s",
                  h, hdaBar.Len, hOk ? "ok" : "UNMAPPED", d, dspBar.Len, dOk ? "ok" : "UNMAPPED",
                  ppcap, pOk ? "ok" : "UNMAPPED");
            CbLog(ctx, "previous mappings: HDA %p, DSP %p. DSP left off.", ctx->LastHdaBar, ctx->LastDspBar);
            ctx->DspBar = NULL;
            ctx->HdaBar = NULL;
            ctx->PpCap = NULL;
            CbFlushLog(ctx);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        ctx->LastHdaBar = h;
        ctx->LastDspBar = d;
    }

    // Our own PPCAP, inside the current HDA BAR. The bus's pointer is only
    // logged for comparison, never used.
    PUCHAR ourPpCap = FindPpCap(ctx, (PUCHAR)hdaBar.Base, hdaBar.Len);
    BOOLEAN busPpCapInBar = ppcap && (PUCHAR)ppcap >= (PUCHAR)hdaBar.Base &&
                            (PUCHAR)ppcap + 0x10 <= (PUCHAR)hdaBar.Base + hdaBar.Len;
    CbLogV(ctx, "HDA BAR %p len 0x%X, DSP BAR %p len 0x%X, PPCAP ours %p bus %p%s",
          hdaBar.Base, hdaBar.Len, dspBar.Base, dspBar.Len, ourPpCap, ppcap,
          busPpCapInBar ? "" : " (bus pointer is OUTSIDE the HDA BAR: stale)");
    if (!ourPpCap || !busPpCapInBar || (PUCHAR)ppcap != ourPpCap) {
        // If the bus's capability pointers are stale, the bus itself would
        // crash in GetRenderStream (reads ppcap) or DSPEnableSPIB (spbcap).
        // Leave the DSP off; restarting the PC re-initializes the bus.
        CbLog(ctx, "HDA capabilities unusable (ours %p, bus %p): DSP left off. Restart the PC to recover.",
              ourPpCap, ppcap);
        ctx->DspBar = NULL;
        ctx->HdaBar = NULL;
        ctx->PpCap = NULL;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (ctx->DspBar && (PUCHAR)dspBar.Base != ctx->DspBar) {
        CbLog(ctx, "bus resources changed: DSP BAR %p -> %p", ctx->DspBar, dspBar.Base);
    }
    ctx->HdaBar = (PUCHAR)hdaBar.Base;
    ctx->HdaBarLen = hdaBar.Len;
    ctx->DspBar = (PUCHAR)dspBar.Base;
    ctx->DspBarLen = dspBar.Len;
    ctx->PpCap = (PUCHAR)ppcap;

    // Re-register every time; the bus forgets the callback when it re-inits.
    status = ctx->Bus.RegisterInterrupt(ctx->Bus.Context, DspInterruptCallback, ctx);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "RegisterInterrupt failed 0x%08X", status);
        return status;
    }
    ctx->InterruptRegistered = TRUE;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Crash guard: "CbsofPowerUpPending" (REG_DWORD, device hardware key) is set
// while a DSP power-up is in progress. If it is still set when we start, the
// last power-up crashed the machine, so leave the DSP off instead of boot
// looping. Delete the value (or reinstall) to try again.
// ---------------------------------------------------------------------------
static ULONG GuardRead(PCBSOF_CONTEXT ctx)
{
    HANDLE key;
    ULONG value = 0;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(ctx->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &key))) {
        return 0;
    }
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"CbsofPowerUpPending");
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    ULONG len = 0;
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, buf, sizeof(buf), &len))) {
        PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
        if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
            value = *(PULONG)info->Data;
        }
    }
    ZwClose(key);
    return value;
}

static void GuardWrite(PCBSOF_CONTEXT ctx, ULONG value)
{
    HANDLE key;
    if (!NT_SUCCESS(IoOpenDeviceRegistryKey(ctx->Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, &key))) {
        return;
    }
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, L"CbsofPowerUpPending");
    if (value) {
        ZwSetValueKey(key, &name, 0, REG_DWORD, &value, sizeof(value));
    } else {
        ZwDeleteValueKey(key, &name);
    }
    ZwFlushKey(key);    // must reach disk before we touch the hardware
    ZwClose(key);
}

NTSTATUS SofDevicePowerUp(void)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (!ctx || !ctx->BusValid) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (ctx->DspRunning) {
        return STATUS_SUCCESS;
    }
    if (ctx->GuardTripped) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (GuardRead(ctx)) {
        ctx->GuardTripped = TRUE;
        CbLog(ctx, "the previous DSP power-up did not finish (crash?). DSP left off to avoid a boot loop.");
        CbLog(ctx, "Delete the CbsofPowerUpPending value (install-on-chromebook.ps1 -ClearGuard) to try again.");
        CbFlushLog(ctx);
        return STATUS_DEVICE_NOT_READY;
    }
    GuardWrite(ctx, 1);

    // Hold the bus in D0 for as long as this device exists (released only in
    // SofDeviceStop). Letting it idle made its next power-up fail
    // (0xC00002D3) and take our device down with it.
    NTSTATUS status = STATUS_SUCCESS;
    if (!ctx->BusHeldD0) {
        status = ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD0);
        CbLog(ctx, "bus held in D0: 0x%08X", status);
        if (!NT_SUCCESS(status)) {
            CbLog(ctx, "the bus refused to power up; DSP left off without touching the hardware");
            GuardWrite(ctx, 0);
            return status;
        }
        ctx->BusHeldD0 = TRUE;
    }

    status = RefreshBusResources(ctx);
    if (!NT_SUCCESS(status)) {
        GuardWrite(ctx, 0);
        CbFlushLog(ctx);
        return status;
    }

    status = DspBoot(ctx);
    CbLogV(ctx, "DspBoot result 0x%08X", status);
    if (NT_SUCCESS(status)) {
        status = DspIpcTest(ctx);
        CbLogV(ctx, "IPC round-trip test 0x%08X", status);
    }
    if (NT_SUCCESS(status)) {
        status = SofLoadTopology(ctx);
    }
    ctx->DspRunning = NT_SUCCESS(status);
    if (ctx->DspRunning) {
        ctx->BootId++;
        if (ctx->BootId > 1) {
            CbLog(ctx, "DSP up again (boot %u)", ctx->BootId);
        }
    } else {
        CbLog(ctx, "DSP power-up failed 0x%08X", status);
    }
    GuardWrite(ctx, 0);     // we survived the hardware access
    CbFlushLog(ctx);
    return status;
}

void SofDevicePowerDown(void)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (!ctx || !ctx->BusValid) {
        return;
    }
    if (ctx->DspBar) {
        if (MmIsAddressValid(ctx->DspBar) && MmIsAddressValid(ctx->PpCap)) {
            DspShutdown(ctx);
        } else {
            CbLog(ctx, "power-down: DSP BAR %p / PPCAP %p no longer mapped; skipping DSP shutdown",
                  ctx->DspBar, ctx->PpCap);
        }
    }
    if (ctx->InterruptRegistered) {
        ctx->Bus.UnregisterInterrupt(ctx->Bus.Context);
        ctx->InterruptRegistered = FALSE;
        KeFlushQueuedDpcs();
    }
    // The bus stays in D0 (see SofDevicePowerUp); only SofDeviceStop releases it.
    ctx->DspRunning = FALSE;
    ctx->TopologyLoaded = FALSE;
    // Forget the mappings: the bus may unmap them while we are down.
    ctx->DspBar = NULL;
    ctx->HdaBar = NULL;
    ctx->PpCap = NULL;
    CbLog(ctx, "DSP powered down");
    CbFlushLog(ctx);
}

NTSTATUS SofDeviceStart(PDEVICE_OBJECT Fdo)
{
    if (g_Cbsof) {
        return STATUS_DEVICE_BUSY;
    }
    PCBSOF_CONTEXT ctx = (PCBSOF_CONTEXT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(CBSOF_CONTEXT), CBSOF_POOL_TAG);
    if (!ctx) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeSpinLock(&ctx->LogLock);
    KeInitializeDpc(&ctx->IpcDpc, DspIpcDpc, ctx);
    KeInitializeEvent(&ctx->FwReadyEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->ReplyEvent, SynchronizationEvent, FALSE);
    KeInitializeMutex(&ctx->IpcMutex, 0);
    ctx->Fdo = Fdo;
    g_Cbsof = ctx;

    CbLog(ctx, "cbsof 0.6.0 (PortCls): starting");

    NTSTATUS status = PcGetPhysicalDeviceObject(Fdo, &ctx->Pdo);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "PcGetPhysicalDeviceObject failed 0x%08X", status);
        return status;
    }
    RotateLog(ctx);
    CbFlushLog(ctx);

    status = QueryAdspInterface(ctx->Pdo, &ctx->Bus);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "ADSP bus interface not found (0x%08X). Is sklhdaudbus installed?", status);
        CbFlushLog(ctx);
        return status;
    }
    ctx->BusValid = TRUE;
    CbLog(ctx, "ADSP bus interface v%u size %u, controller DEV_%04X",
          ctx->Bus.Version, ctx->Bus.Size, ctx->Bus.CtlrDevId);
    if (ctx->Bus.Size < sizeof(ADSP_BUS_INTERFACE) || ctx->Bus.CtlrDevId != 0x3198) {
        CbLog(ctx, "unsupported bus interface or controller");
        CbFlushLog(ctx);
        return STATUS_NOT_SUPPORTED;
    }

    ctx->Instance = (ULONG)InterlockedIncrement(&g_InstanceCounter);

    CsAudioOpen(ctx);

    // Headphone jack detection from the DA7219 driver (independent of the DSP)
    SofJackStart(Fdo->DriverObject);

    // A DSP failure is logged but does not fail the device, so the endpoints
    // still appear and the log stays readable.
    SofDevicePowerUp();
    return STATUS_SUCCESS;
}

void SofDeviceStop(void)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (!ctx) {
        return;
    }
    CbLog(ctx, "device stopping");
    SofJackStop();
    SofDevicePowerDown();
    if (ctx->BusHeldD0 && ctx->BusValid) {
        ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD3);    // let the bus idle again
        ctx->BusHeldD0 = FALSE;
    }
    CsAudioClose(ctx);
    if (ctx->BusValid && ctx->Bus.InterfaceDereference) {
        ctx->Bus.InterfaceDereference(ctx->Bus.Context);
    }
    ctx->BusValid = FALSE;
    g_Cbsof = NULL;
    ExFreePoolWithTag(ctx, CBSOF_POOL_TAG);
}
