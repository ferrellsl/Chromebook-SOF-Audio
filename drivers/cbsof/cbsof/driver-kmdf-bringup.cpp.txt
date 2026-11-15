// cbsof: WDF glue for the milestone 2 bring-up driver.
#include "driver.h"
#include <ntstrsafe.h>
#include <stdarg.h>

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD CbEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE CbEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE CbEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY CbEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT CbEvtD0Exit;

// ---------------------------------------------------------------------------
// Logging: DbgPrint plus an in-memory log that is written to the device's
// hardware registry key as "CbsofLog" (REG_SZ).
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
    if (ctx->LogLen + len + 2 >= CBSOF_LOG_SIZE) {
        return;
    }
    RtlCopyMemory(ctx->Log + ctx->LogLen, line, len);
    ctx->LogLen += (ULONG)len;
    ctx->Log[ctx->LogLen++] = '\r';
    ctx->Log[ctx->LogLen++] = '\n';
    ctx->Log[ctx->LogLen] = 0;
}

void CbFlushLog(PCBSOF_CONTEXT ctx)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    WDFKEY key;
    NTSTATUS status = WdfDeviceOpenRegistryKey(ctx->Device, PLUGPLAY_REGKEY_DEVICE,
                                               KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) {
        return;
    }

    ANSI_STRING ansi;
    RtlInitAnsiString(&ansi, ctx->Log);
    UNICODE_STRING value;
    if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&value, &ansi, TRUE))) {
        DECLARE_CONST_UNICODE_STRING(name, L"CbsofLog");
        WdfRegistryAssignUnicodeString(key, &name, &value);
        RtlFreeUnicodeString(&value);
    }
    WdfRegistryClose(key);
}

// ---------------------------------------------------------------------------
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, CbEvtDeviceAdd);
    CbLog(NULL, "DriverEntry");
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS CbEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = CbEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = CbEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = CbEvtD0Entry;
    pnp.EvtDeviceD0Exit = CbEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CBSOF_CONTEXT);

    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        CbLog(NULL, "WdfDeviceCreate failed 0x%08X", status);
        return status;
    }

    PCBSOF_CONTEXT ctx = GetCbsofContext(device);
    ctx->Device = device;
    KeInitializeDpc(&ctx->IpcDpc, DspIpcDpc, ctx);
    KeInitializeEvent(&ctx->FwReadyEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->ReplyEvent, SynchronizationEvent, FALSE);

    // Never let the bus power this device down while we bring it up.
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idle;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idle, IdleCannotWakeFromS0);
    idle.UserControlOfIdleSettings = IdleDoNotAllowUserControl;
    idle.Enabled = WdfFalse;
    WdfDeviceAssignS0IdleSettings(device, &idle);

    return STATUS_SUCCESS;
}

NTSTATUS CbEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    PCBSOF_CONTEXT ctx = GetCbsofContext(Device);

    NTSTATUS status = WdfFdoQueryForInterface(Device, &GUID_ADSP_BUS_INTERFACE,
                                              (PINTERFACE)&ctx->Bus, sizeof(ADSP_BUS_INTERFACE),
                                              1, NULL);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "ADSP bus interface not found (0x%08X). Is sklhdaudbus installed?", status);
        CbFlushLog(ctx);
        return status;
    }
    ctx->BusValid = TRUE;
    CbLog(ctx, "ADSP bus interface v%u size %u, controller DEV_%04X",
          ctx->Bus.Version, ctx->Bus.Size, ctx->Bus.CtlrDevId);

    if (ctx->Bus.Size < sizeof(ADSP_BUS_INTERFACE)) {
        CbLog(ctx, "bus interface too small (%u < %u)", ctx->Bus.Size, (ULONG)sizeof(ADSP_BUS_INTERFACE));
        CbFlushLog(ctx);
        return STATUS_NOT_SUPPORTED;
    }
    if (ctx->Bus.CtlrDevId != 0x3198) {
        CbLog(ctx, "only Gemini Lake (DEV_3198) is supported so far");
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
    CbLog(ctx, "HDA BAR %p len 0x%X, DSP BAR %p len 0x%X, PPCAP %p, NHLT %p size %llu",
          ctx->HdaBar, ctx->HdaBarLen, ctx->DspBar, ctx->DspBarLen, ctx->PpCap,
          nhlt.nhlt, nhlt.nhltSz);

    if (!ctx->DspBar || ctx->DspBarLen < DSP_MBOX_UPLINK_OFFSET + 0x1000 || !ctx->PpCap) {
        CbLog(ctx, "DSP BAR or PPCAP missing; is the controller in DSP mode?");
        CbFlushLog(ctx);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (nhlt.nhlt && nhlt.nhltSz >= 4) {
        CbLog(ctx, "NHLT/TPLG blob signature %.4s", (const char*)nhlt.nhlt);
    }

    status = ctx->Bus.RegisterInterrupt(ctx->Bus.Context, DspInterruptCallback, ctx);
    if (!NT_SUCCESS(status)) {
        CbLog(ctx, "RegisterInterrupt failed 0x%08X", status);
        CbFlushLog(ctx);
        return status;
    }
    ctx->InterruptRegistered = TRUE;

    // Not fatal: without it the codec drivers just won't power the amp.
    CsAudioOpen(ctx);

    CbFlushLog(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS CbEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    PCBSOF_CONTEXT ctx = GetCbsofContext(Device);
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
    return STATUS_SUCCESS;
}

NTSTATUS CbEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    PCBSOF_CONTEXT ctx = GetCbsofContext(Device);

    NTSTATUS status = ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD0);
    CbLog(ctx, "SetDSPPowerState(D0) 0x%08X", status);

    status = DspBoot(ctx);
    CbLog(ctx, "DspBoot result 0x%08X", status);
    if (NT_SUCCESS(status)) {
        NTSTATUS ipc = DspIpcTest(ctx);
        CbLog(ctx, "IPC round-trip test 0x%08X", ipc);
        CbFlushLog(ctx);
        if (NT_SUCCESS(ipc)) {
            NTSTATUS tone = DspSpeakerToneTest(ctx);
            CbLog(ctx, "speaker tone test 0x%08X", tone);
        }
    }
    CbFlushLog(ctx);

    // Bring-up: never fail D0 entry, so the log stays readable and the
    // device can be disabled/re-enabled to retry.
    return STATUS_SUCCESS;
}

NTSTATUS CbEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    PCBSOF_CONTEXT ctx = GetCbsofContext(Device);
    DspShutdown(ctx);
    ctx->Bus.SetDSPPowerState(ctx->Bus.Context, PowerDeviceD3);
    CbLog(ctx, "D0Exit: DSP shut down");
    CbFlushLog(ctx);
    return STATUS_SUCCESS;
}
