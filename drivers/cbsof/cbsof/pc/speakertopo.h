
/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    speakertopo.h

Abstract:

    Declaration of topology miniport for the speaker (internal).
--*/

#ifndef _SIMPLEAUDIOSAMPLE_SPEAKERTOPO_H_
#define _SIMPLEAUDIOSAMPLE_SPEAKERTOPO_H_

NTSTATUS PropertyHandler_SpeakerTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);

NTSTATUS PropertyHandler_SpeakerTopology(_In_ PPCPROPERTY_REQUEST PropertyRequest);

// cbsof: jack detection (speakers report unplugged while headphones are in)
NTSTATUS EventHandler_SpeakerJack(_In_ PPCEVENT_REQUEST EventRequest);
VOID SpeakerJackChanged(_In_ PVOID Context, _In_ BOOLEAN Connected);

#endif // _SIMPLEAUDIOSAMPLE_SPEAKERTOPO_H_
