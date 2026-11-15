/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    speakertopo.cpp

Abstract:

    Implementation of topology miniport for the speaker (internal).
--*/

#pragma warning (disable : 4127)

#include "definitions.h"
#include "endpoints.h"
#include "mintopo.h"
#include "speakertopo.h"
#include "speakertoptable.h"
#include "..\sofapi.h"


#pragma code_seg("PAGE")
//=============================================================================
NTSTATUS
PropertyHandler_SpeakerTopoFilter
( 
    _In_ PPCPROPERTY_REQUEST      PropertyRequest 
)
/*++

Routine Description:

  Redirects property request to miniport object

Arguments:

  PropertyRequest - 

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_SpeakerTopoFilter]"));

    // PropertryRequest structure is filled by portcls. 
    // MajorTarget is a pointer to miniport object for miniports.
    //
    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    PCMiniportTopology  pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack))
    {
        // cbsof: speakers count as unplugged while headphones are in, so
        // Windows moves playback to the headphones and back (sofjack.cpp)
        SpeakerJackDescBridge.IsConnected = SofSpeakerConnected();

        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription(
                PropertyRequest,
                ARRAYSIZE(SpeakerJackDescriptions),
                SpeakerJackDescriptions
                );
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription2(
                PropertyRequest,
                ARRAYSIZE(SpeakerJackDescriptions),
                SpeakerJackDescriptions,
                JACKDESC2_PRESENCE_DETECT_CAPABILITY
                );
        }
    }

    return ntStatus;
} // PropertyHandler_SpeakerTopoFilter

//=============================================================================
// cbsof: jack-change event on the speaker pin (see headphonetopo.cpp)
NTSTATUS
EventHandler_SpeakerJack
(
    _In_ PPCEVENT_REQUEST EventRequest
)
{
    PAGED_CODE();
    PCMiniportTopology pMiniport = (PCMiniportTopology)EventRequest->MajorTarget;
    SofTrace("%s jack event: verb %u", "speaker", (ULONG)EventRequest->Verb);
    if (EventRequest->Verb == PCEVENT_VERB_ADD) {
        pMiniport->AddEventToEventList(EventRequest->EventEntry);
    }
    return STATUS_SUCCESS;
}

#pragma code_seg()
VOID
SpeakerJackChanged
(
    _In_ PVOID   Context,
    _In_ BOOLEAN Connected
)
{
    PCMiniportTopology pMiniport = (PCMiniportTopology)Context;
    SofTrace("raising speaker jack change (connected %u)", (ULONG)Connected);
    pMiniport->GenerateEventList(
        (GUID*)&KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        TRUE,
        KSPIN_TOPO_LINEOUT_DEST,
        FALSE,
        ULONG(-1));
}
#pragma code_seg("PAGE")

//=============================================================================
NTSTATUS
PropertyHandler_SpeakerTopology
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
/*++

Routine Description:

  Redirects property request to miniport object

Arguments:

  PropertyRequest -

Return Value:

  NT status code.

--*/
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_SpeakerTopology]"));

    // PropertryRequest structure is filled by portcls. 
    // MajorTarget is a pointer to miniport object for miniports.
    //
    PCMiniportTopology pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    return pMiniport->PropertyHandlerGeneric(PropertyRequest);
} // PropertyHandler_SpeakerTopology

#pragma code_seg()
