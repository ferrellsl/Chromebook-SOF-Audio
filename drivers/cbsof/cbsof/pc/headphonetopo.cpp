/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    headphonetopo.cpp

Abstract:

    Implementation of topology miniport for the headphone (internal).
--*/

#pragma warning (disable : 4127)

#include "definitions.h"
#include "endpoints.h"
#include "mintopo.h"
#include "headphonetopo.h"
#include "headphonetoptable.h"
#include "..\sofapi.h"


#pragma code_seg("PAGE")
//=============================================================================
NTSTATUS
PropertyHandler_HeadphoneTopoFilter
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

    DPF_ENTER(("[PropertyHandler_HeadphoneTopoFilter]"));

    // PropertryRequest structure is filled by portcls. 
    // MajorTarget is a pointer to miniport object for miniports.
    //
    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    PCMiniportTopology  pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack))
    {
        // cbsof: the live jack state from the DA7219 (sofjack.cpp)
        HeadphoneJackDescBridge.IsConnected = SofHeadphoneConnected();

        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription(
                PropertyRequest,
                ARRAYSIZE(HeadphoneJackDescriptions),
                HeadphoneJackDescriptions
                );
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription2(
                PropertyRequest,
                ARRAYSIZE(HeadphoneJackDescriptions),
                HeadphoneJackDescriptions,
                JACKDESC2_PRESENCE_DETECT_CAPABILITY // the DA7219 detects plug/unplug
                );
        }
    }

    return ntStatus;
} // PropertyHandler_HeadphoneTopoFilter

//=============================================================================
// cbsof: KSEVENT_PINCAPS_JACKINFOCHANGE on the headphone jack pin. Windows
// enables it; we raise it from the jack callback below.
NTSTATUS
EventHandler_HeadphoneJack
(
    _In_ PPCEVENT_REQUEST EventRequest
)
{
    PAGED_CODE();
    PCMiniportTopology pMiniport = (PCMiniportTopology)EventRequest->MajorTarget;
    SofTrace("%s jack event: verb %u", "headphone", (ULONG)EventRequest->Verb);
    if (EventRequest->Verb == PCEVENT_VERB_ADD) {
        pMiniport->AddEventToEventList(EventRequest->EventEntry);
    }
    // PCEVENT_VERB_REMOVE / SUPPORT: nothing to do
    return STATUS_SUCCESS;
}

#pragma code_seg()
//=============================================================================
// Called by sofjack.cpp (PASSIVE_LEVEL) when the jack state changes
VOID
HeadphoneJackChanged
(
    _In_ PVOID   Context,
    _In_ BOOLEAN Connected
)
{
    PCMiniportTopology pMiniport = (PCMiniportTopology)Context;
    SofTrace("raising headphone jack change (connected %u)", (ULONG)Connected);
    pMiniport->GenerateEventList(
        (GUID*)&KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        TRUE,                       // pin event
        KSPIN_TOPO_LINEOUT_DEST,    // the jack pin
        FALSE,
        ULONG(-1));
}
#pragma code_seg("PAGE")

//=============================================================================
NTSTATUS
PropertyHandler_HeadphoneTopology
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

    DPF_ENTER(("[PropertyHandler_HeadphoneTopology]"));

    // PropertryRequest structure is filled by portcls. 
    // MajorTarget is a pointer to miniport object for miniports.
    //
    PCMiniportTopology pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    return pMiniport->PropertyHandlerGeneric(PropertyRequest);
} // PropertyHandler_HeadphoneTopology

#pragma code_seg()
