// cbsof: DSP device lifetime, owned by the PortCls adapter (CAdapterCommon).
#include "driver.h"
#include <ntstrsafe.h>
#include <stdarg.h>

extern "C" NTSTATUS PcGetPhysicalDeviceObject(PDEVICE_OBJECT DeviceObject, PDEVICE_OBJECT* PhysicalDeviceObject);

PCBSOF_CONTEXT g_Cbsof = NULL;

// ---------------------------------------------------------------------------
// Logging: DbgPrint plus an in-memory log written to the device's hardware
// key as "CbsofLog" (REG_SZ).
// ---------------------------------------------------------------------------
void CbLog(PCBSOF_CONTEXT ctx, const char* fmt, ...)
{
    CHAR line[256];
    va_list args;
    va_start(args, fmt);
    NTSTATUS status = RtlStringCbVPrintfA(line, sizeof(line), fmt, args);
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
NTSTATUS SofDevicePowerUp(void)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (!ctx || !ctx->BusValid) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (ctx->DspRunning) {
        return STATUS_SUCCESS;
    }

    NTSTATUS status = ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD0);
    CbLog(ctx, "SetDSPPowerState(D0) 0x%08X", status);

    status = DspBoot(ctx);
    CbLog(ctx, "DspBoot result 0x%08X", status);
    if (NT_SUCCESS(status)) {
        status = DspIpcTest(ctx);
        CbLog(ctx, "IPC round-trip test 0x%08X", status);
    }
    if (NT_SUCCESS(status)) {
        status = SofLoadTopology(ctx);
    }
    ctx->DspRunning = NT_SUCCESS(status);
    CbFlushLog(ctx);
    return status;
}

void SofDevicePowerDown(void)
{
    PCBSOF_CONTEXT ctx = g_Cbsof;
    if (!ctx || !ctx->BusValid) {
        return;
    }
    DspShutdown(ctx);
    if (ctx->DspRunning) {
        ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD3);
    }
    ctx->DspRunning = FALSE;
    ctx->TopologyLoaded = FALSE;
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

    CbLog(ctx, "cbsof 0.4 (PortCls): starting");

    NTSTATUS status = PcGetPhysicalDeviceObject(Fdo, &ctx->Pdo);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "PcGetPhysicalDeviceObject failed 0x%08X", status);
        return status;
    }

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

    CB_PCI_BAR hdaBar = {}, dspBar = {};
    PVOID ppcap = NULL;
    NHLT_INFO nhlt = {};
    BUS_INTERFACE_STANDARD pci = {};
    status = ctx->Bus.GetResources(ctx->Bus.Context, &hdaBar, &dspBar, &ppcap, &nhlt, &pci);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "GetResources failed 0x%08X", status);
        CbFlushLog(ctx);
        return status;
    }
    ctx->HdaBar = (PUCHAR)hdaBar.Base;
    ctx->HdaBarLen = hdaBar.Len;
    ctx->DspBar = (PUCHAR)dspBar.Base;
    ctx->DspBarLen = dspBar.Len;
    ctx->PpCap = (PUCHAR)ppcap;
    if (!ctx->HdaBar || !ctx->DspBar || ctx->DspBarLen < DSP_MBOX_UPLINK_OFFSET + 0x1000 || !ctx->PpCap) {
        CbLog(ctx, "DSP BAR or PPCAP missing");
        CbFlushLog(ctx);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    status = ctx->Bus.RegisterInterrupt(ctx->Bus.Context, DspInterruptCallback, ctx);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "RegisterInterrupt failed 0x%08X", status);
        CbFlushLog(ctx);
        return status;
    }
    ctx->InterruptRegistered = TRUE;

    CsAudioOpen(ctx);

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
    SofDevicePowerDown();
    CsAudioClose(ctx);
    if (ctx->InterruptRegistered) {
        ctx->Bus.UnregisterInterrupt(ctx->Bus.Context);
        ctx->InterruptRegistered = FALSE;
        KeFlushQueuedDpcs();
    }
    if (ctx->BusValid && ctx->Bus.InterfaceDereference) {
        ctx->Bus.InterfaceDereference(ctx->Bus.Context);
    }
    ctx->BusValid = FALSE;
    g_Cbsof = NULL;
    ExFreePoolWithTag(ctx, CBSOF_POOL_TAG);
}
