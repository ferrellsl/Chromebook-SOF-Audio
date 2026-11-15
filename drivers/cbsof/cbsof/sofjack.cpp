// cbsof: headphone jack detection.
//
// Coolstar's DA7219 driver (coolstar/da7219, Apache-2.0) reports the jack as
// a HID device (HID\DLGS7219&COL01/COL02 on Bobba). Its second (vendor, usage page 0xFF00)
// collection carries input report 2 = { 2, ControlCode, ControlValue } with
// ControlCode 1 = jack type (0 = nothing, bit 0 = headphone, bit 1 = mic),
// and writing output report { 2, x } makes it re-send the current state.
//
// A PnP notification finds that collection; a system thread opens it and
// reads reports; changes go to the headphone topology miniport, which raises
// KSEVENT_PINCAPS_JACKINFOCHANGE so Windows switches endpoints itself.
#include "driver.h"
#include <initguid.h>
#include <hidclass.h>
#include <wdmguid.h>
#include <ntstrsafe.h>

// From ntifs.h (exported by ntoskrnl); declared here to avoid mixing ntifs.h
// with the ntddk.h-based headers.
extern "C" {
NTSYSAPI NTSTATUS NTAPI ZwCreateEvent(PHANDLE EventHandle, ACCESS_MASK DesiredAccess,
                                      POBJECT_ATTRIBUTES ObjectAttributes, EVENT_TYPE EventType,
                                      BOOLEAN InitialState);
NTSYSAPI NTSTATUS NTAPI ZwClearEvent(HANDLE EventHandle);
NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
}

#define DA7219_REPORTID_SPECKEYS   0x02
#define DA7219_CONTROL_JACK_TYPE   0x01
#define DA7219_JACK_HEADPHONE      0x01
#define DA7219_IN_REPORT_BYTES     3
#define DA7219_OUT_REPORT_BYTES    2

static PVOID g_NotifyHandle = NULL;
static KGUARDED_MUTEX g_JackLock;            // callback, thread handle, file handle
static BOOLEAN g_LockInit = FALSE;
static SOF_JACK_CALLBACK g_Callback[2] = {};
static PVOID g_CallbackContext[2] = {};
// Until the DA7219 tells us otherwise, report "connected", so headphones
// still work (as in 0.5.0) if the jack device is never found.
static volatile LONG g_Connected = 1;
static volatile LONG g_KnownState = 0;
static HANDLE g_ThreadHandle = NULL;
static PKTHREAD g_Thread = NULL;
static HANDLE g_FileHandle = NULL;
static volatile LONG g_Stopping = 0;
static WCHAR g_LinkName[256];
static KEVENT g_StopEvent;                   // set by SofJackStop; wakes a waiting read

static void JackLog(const char* fmt, ...)
{
    CHAR line[200];
    va_list args;
    va_start(args, fmt);
    RtlStringCbVPrintfA(line, sizeof(line), fmt, args);
    va_end(args);
    CbLog(g_Cbsof, "jack: %s", line);
    CbSetStatus(g_Cbsof, L"CbsofJack", line);   // latest jack status, never scrolls out
}

// Until the DA7219 has reported (or if it never does) both outputs count as
// connected, so neither becomes unusable.
BOOLEAN SofHeadphoneConnected(void)
{
    return !g_KnownState || g_Connected != 0;
}

BOOLEAN SofSpeakerConnected(void)
{
    return !g_KnownState || g_Connected == 0;
}

void SofSetJackCallback(ULONG Slot, SOF_JACK_CALLBACK Callback, PVOID Context)
{
    if (Slot > 1) {
        return;
    }
    if (!g_LockInit) {
        KeInitializeGuardedMutex(&g_JackLock);
        KeInitializeEvent(&g_StopEvent, NotificationEvent, FALSE);
        g_LockInit = TRUE;
    }
    KeAcquireGuardedMutex(&g_JackLock);
    g_Callback[Slot] = Callback;
    g_CallbackContext[Slot] = Context;
    KeReleaseGuardedMutex(&g_JackLock);
}

static void SetJackState(BOOLEAN connected)
{
    LONG old = InterlockedExchange(&g_Connected, connected ? 1 : 0);
    LONG first = InterlockedExchange(&g_KnownState, 1) == 0;
    if (old == (connected ? 1 : 0) && !first) {
        return;
    }
    JackLog("headphones %s", connected ? "plugged in" : "unplugged");
    KeAcquireGuardedMutex(&g_JackLock);
    for (int i = 0; i < 2; i++) {
        if (g_Callback[i]) {
            g_Callback[i](g_CallbackContext[i], connected);
        }
    }
    KeReleaseGuardedMutex(&g_JackLock);
}

// Case-insensitive "does haystack contain needle" for UNICODE_STRINGs
static BOOLEAN ContainsNoCase(PCUNICODE_STRING hay, PCWSTR needle)
{
    size_t n = wcslen(needle);
    USHORT h = hay->Length / sizeof(WCHAR);
    for (USHORT i = 0; i + n <= h; i++) {
        size_t k = 0;
        for (; k < n; k++) {
            if (RtlUpcaseUnicodeChar(hay->Buffer[i + k]) != RtlUpcaseUnicodeChar(needle[k])) {
                break;
            }
        }
        if (k == n) {
            return TRUE;
        }
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// One report read with our own IRP, so a stop can cancel it. If writeHandle is
// given, the state request is sent once the read is waiting for the answer.
// ---------------------------------------------------------------------------
static NTSTATUS ReadReport(PDEVICE_OBJECT dev, PFILE_OBJECT fo, PUCHAR buf, ULONG len, PULONG got,
                           HANDLE writeHandle, HANDLE writeEvent)
{
    *got = 0;
    KEVENT done;
    KeInitializeEvent(&done, NotificationEvent, FALSE);
    IO_STATUS_BLOCK iosb = {};
    LARGE_INTEGER offset = {};
    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ, dev, buf, len, &offset, &done, &iosb);
    if (!irp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    IoGetNextIrpStackLocation(irp)->FileObject = fo;
    NTSTATUS status = IoCallDriver(dev, irp);
    if (status == STATUS_PENDING) {
        if (writeHandle) {
            UCHAR req[DA7219_OUT_REPORT_BYTES] = { DA7219_REPORTID_SPECKEYS, 0 };
            IO_STATUS_BLOCK wiosb = {};
            LARGE_INTEGER woff = {};
            ZwClearEvent(writeEvent);
            NTSTATUS ws = ZwWriteFile(writeHandle, writeEvent, NULL, NULL, &wiosb, req, sizeof(req), &woff, NULL);
            if (ws == STATUS_PENDING) {
                LARGE_INTEGER t;
                t.QuadPart = -10LL * 1000 * 1000;   // 1 s
                ws = ZwWaitForSingleObject(writeEvent, FALSE, &t) == STATUS_TIMEOUT ? STATUS_TIMEOUT : wiosb.Status;
            }
            if (!NT_SUCCESS(ws) || ws == STATUS_TIMEOUT) {
                JackLog("state request failed 0x%08X (state known after the next plug event)", ws);
            }
        }
        PVOID objs[2] = { &done, &g_StopEvent };
        NTSTATUS w = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        if (w == STATUS_WAIT_1) {
            // Stopping: cancel our read and wait for it to come back (the IRP
            // points at locals, so it must finish before we return).
            IoCancelIrp(irp);
            KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
        }
        status = iosb.Status;
    }
    if (NT_SUCCESS(status)) {
        *got = (ULONG)iosb.Information;
    }
    return status;
}

// ---------------------------------------------------------------------------
// Listener thread: open the collection, ask for the current state, read reports
// ---------------------------------------------------------------------------
static KSTART_ROUTINE JackThread;
static VOID JackThread(PVOID StartContext)
{
    UNREFERENCED_PARAMETER(StartContext);
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, g_LinkName);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    IO_STATUS_BLOCK iosb;
    HANDLE file = NULL;

    NTSTATUS status = ZwCreateFile(&file, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL, 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status)) {
        JackLog("cannot open the DA7219 jack device (0x%08X)", status);
        goto out;
    }
    KeAcquireGuardedMutex(&g_JackLock);
    g_FileHandle = file;
    KeReleaseGuardedMutex(&g_JackLock);
    JackLog("listening to the DA7219 jack device");

    {
        // Reads use our own IRP so a stop can cancel them (0.5.2 hung the
        // device stop: closing the handle does not cancel a pending HID read).
        PFILE_OBJECT fo = NULL;
        status = ObReferenceObjectByHandle(file, 0, *IoFileObjectType, KernelMode, (PVOID*)&fo, NULL);
        if (!NT_SUCCESS(status)) {
            JackLog("cannot reference the jack device file (0x%08X)", status);
            goto out;
        }
        PDEVICE_OBJECT dev = IoGetRelatedDeviceObject(fo);
        PUCHAR report = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, 16, CBSOF_POOL_TAG);
        HANDLE writeEvent = NULL;
        OBJECT_ATTRIBUTES eoa;
        InitializeObjectAttributes(&eoa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
        if (!report || !NT_SUCCESS(ZwCreateEvent(&writeEvent, EVENT_ALL_ACCESS, &eoa, NotificationEvent, FALSE))) {
            JackLog("out of resources");
            if (report) ExFreePoolWithTag(report, CBSOF_POOL_TAG);
            ObDereferenceObject(fo);
            goto out;
        }
        BOOLEAN asked = FALSE;

        while (!g_Stopping) {
            RtlZeroMemory(report, 16);
            ULONG got = 0;
            status = ReadReport(dev, fo, report, DA7219_IN_REPORT_BYTES, &got,
                                asked ? NULL : file, asked ? NULL : writeEvent);
            asked = TRUE;
            if (status == STATUS_CANCELLED && g_Stopping) {
                break;
            }
            if (!NT_SUCCESS(status)) {
                JackLog("read ended 0x%08X (device removed?)", status);
                break;
            }
            JackLog("report %02X %02X %02X (%u bytes)", report[0], report[1], report[2], got);
            if (got >= DA7219_IN_REPORT_BYTES &&
                report[0] == DA7219_REPORTID_SPECKEYS && report[1] == DA7219_CONTROL_JACK_TYPE) {
                SetJackState((report[2] & DA7219_JACK_HEADPHONE) != 0);
            }
        }
        ZwClose(writeEvent);
        ExFreePoolWithTag(report, CBSOF_POOL_TAG);
        ObDereferenceObject(fo);
    }

out:
    KeAcquireGuardedMutex(&g_JackLock);
    if (g_FileHandle) {
        ZwClose(g_FileHandle);
        g_FileHandle = NULL;
    }
    KeReleaseGuardedMutex(&g_JackLock);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static void StartThread(void)
{
    if (g_ThreadHandle || g_Stopping) {
        return;
    }
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE h;
    if (NT_SUCCESS(PsCreateSystemThread(&h, THREAD_ALL_ACCESS, &oa, NULL, NULL, JackThread, NULL))) {
        g_ThreadHandle = h;
        ObReferenceObjectByHandle(h, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID*)&g_Thread, NULL);
    }
}

// Waits for a previous listener thread (e.g. after the HID device went away)
static void ReapThread(void)
{
    if (g_Thread) {
        KeWaitForSingleObject(g_Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_Thread);
        g_Thread = NULL;
    }
    if (g_ThreadHandle) {
        ZwClose(g_ThreadHandle);
        g_ThreadHandle = NULL;
    }
}

static DRIVER_NOTIFICATION_CALLBACK_ROUTINE JackPnpCallback;
static NTSTATUS JackPnpCallback(PVOID NotificationStructure, PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION n = (PDEVICE_INTERFACE_CHANGE_NOTIFICATION)NotificationStructure;
    if (!IsEqualGUID(n->Event, GUID_DEVICE_INTERFACE_ARRIVAL) || g_Stopping) {
        return STATUS_SUCCESS;
    }
    BOOLEAN related = ContainsNoCase(n->SymbolicLinkName, L"2DCF") ||
                      ContainsNoCase(n->SymbolicLinkName, L"7219") ||
                      ContainsNoCase(n->SymbolicLinkName, L"DLGS");
    if (related) {
        CHAR name[200];
        RtlStringCbPrintfA(name, sizeof(name), "%wZ", n->SymbolicLinkName);
        JackLog("HID interface seen: %s", name);
    }
    // The vendor collection of the DA7219 HID device is its second one. On
    // Bobba it is named after the ACPI ID: HID\DLGS7219&COL02 (seen
    // 2026-11-14); VID_2DCF&PID_7219 is accepted too in case another build names it that way.
    if (!(ContainsNoCase(n->SymbolicLinkName, L"DLGS7219") ||
          ContainsNoCase(n->SymbolicLinkName, L"VID_2DCF&PID_7219")) ||
        !ContainsNoCase(n->SymbolicLinkName, L"COL02")) {
        return STATUS_SUCCESS;
    }
    JackLog("matched the DA7219 vendor collection");
    if (n->SymbolicLinkName->Length >= sizeof(g_LinkName)) {
        return STATUS_SUCCESS;
    }
    // A thread from an earlier arrival has finished once its file handle is gone.
    if (g_ThreadHandle && g_FileHandle == NULL) {
        ReapThread();
    }
    if (g_ThreadHandle) {
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(g_LinkName, sizeof(g_LinkName));
    RtlCopyMemory(g_LinkName, n->SymbolicLinkName->Buffer, n->SymbolicLinkName->Length);
    StartThread();
    return STATUS_SUCCESS;
}

NTSTATUS SofJackStart(PDRIVER_OBJECT DriverObject)
{
    if (!g_LockInit) {
        KeInitializeGuardedMutex(&g_JackLock);
        KeInitializeEvent(&g_StopEvent, NotificationEvent, FALSE);
        g_LockInit = TRUE;
    }
    if (g_NotifyHandle) {
        return STATUS_SUCCESS;
    }
    g_Stopping = 0;
    KeClearEvent(&g_StopEvent);
    NTSTATUS status = IoRegisterPlugPlayNotification(EventCategoryDeviceInterfaceChange,
                                                     PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
                                                     (PVOID)&GUID_DEVINTERFACE_HID, DriverObject,
                                                     JackPnpCallback, NULL, &g_NotifyHandle);
    if (!NT_SUCCESS(status)) {
        JackLog("IoRegisterPlugPlayNotification failed 0x%08X; no jack detection", status);
        g_NotifyHandle = NULL;
    } else if (!g_ThreadHandle) {
        JackLog("watching for the DA7219 jack device (not present yet)");
    }
    return status;
}

void SofJackStop(void)
{
    if (!g_LockInit) {
        return;
    }
    InterlockedExchange(&g_Stopping, 1);
    if (g_NotifyHandle) {
        IoUnregisterPlugPlayNotificationEx(g_NotifyHandle);
        g_NotifyHandle = NULL;
    }
    // Wake the listener; it cancels its own pending read and exits.
    KeSetEvent(&g_StopEvent, IO_NO_INCREMENT, FALSE);
    if (g_Thread) {
        LARGE_INTEGER t;
        t.QuadPart = -50LL * 1000 * 1000;   // 5 s
        if (KeWaitForSingleObject(g_Thread, Executive, KernelMode, FALSE, &t) == STATUS_TIMEOUT) {
            // Unloading with the thread alive would crash, so keep waiting,
            // but leave a trace in the log first.
            JackLog("listener did not stop within 5 s; still waiting");
        }
    }
    ReapThread();
    JackLog("stopped");
}
