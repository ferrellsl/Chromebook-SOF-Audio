/*++

cbsof: capture topology handlers (built-in DMIC, headset mic).

Same pattern as speakertopo.cpp / headphonetopo.cpp (from Microsoft
SimpleAudioSample, MS-PL): the jack state comes from sofjack.cpp; while a
headset mic is plugged in the built-in mics report "unplugged" so Windows
moves recording to the headset and back.
--*/

#pragma warning (disable : 4127)

#include "definitions.h"
#include "endpoints.h"
#include "mintopo.h"
#include "captopo.h"
#include "captoptable.h"
#include "..\sofapi.h"

#pragma code_seg("PAGE")
static NTSTATUS CaptureJackProperty(PPCPROPERTY_REQUEST PropertyRequest, PKSJACK_DESCRIPTION* descs, ULONG count)
{
    PCMiniportTopology pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;
    if (!IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack)) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION) {
        return pMiniport->PropertyHandlerJackDescription(PropertyRequest, count, descs);
    }
    if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2) {
        return pMiniport->PropertyHandlerJackDescription2(PropertyRequest, count, descs,
                                                          JACKDESC2_PRESENCE_DETECT_CAPABILITY);
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS PropertyHandler_DmicTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    DmicJackDesc.IsConnected = SofDmicConnected();
    return CaptureJackProperty(PropertyRequest, DmicJackDescriptions, ARRAYSIZE(DmicJackDescriptions));
}

NTSTATUS PropertyHandler_MicInTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    MicInJackDesc.IsConnected = SofHeadsetMicConnected();
    return CaptureJackProperty(PropertyRequest, MicInJackDescriptions, ARRAYSIZE(MicInJackDescriptions));
}

static NTSTATUS CaptureJackEvent(PPCEVENT_REQUEST EventRequest, const char* name)
{
    PCMiniportTopology pMiniport = (PCMiniportTopology)EventRequest->MajorTarget;
    if (EventRequest->Verb == PCEVENT_VERB_ADD) {
        SofTrace("%s jack event subscribed", name);
        pMiniport->AddEventToEventList(EventRequest->EventEntry);
    }
    return STATUS_SUCCESS;
}

NTSTATUS EventHandler_DmicJack(_In_ PPCEVENT_REQUEST EventRequest)
{
    PAGED_CODE();
    return CaptureJackEvent(EventRequest, "built-in mic");
}

NTSTATUS EventHandler_MicInJack(_In_ PPCEVENT_REQUEST EventRequest)
{
    PAGED_CODE();
    return CaptureJackEvent(EventRequest, "headset mic");
}

#pragma code_seg()
static VOID RaiseMicJackChange(PVOID Context)
{
    PCMiniportTopology pMiniport = (PCMiniportTopology)Context;
    pMiniport->GenerateEventList(
        (GUID*)&KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        TRUE,                       // pin event
        KSPIN_TOPO_MIC_ELEMENTS,    // the jack pin
        FALSE,
        ULONG(-1));
}

VOID DmicJackChanged(_In_ PVOID Context, _In_ BOOLEAN Connected)
{
    UNREFERENCED_PARAMETER(Connected);
    RaiseMicJackChange(Context);
}

VOID MicInJackChanged(_In_ PVOID Context, _In_ BOOLEAN Connected)
{
    UNREFERENCED_PARAMETER(Connected);
    RaiseMicJackChange(Context);
}
