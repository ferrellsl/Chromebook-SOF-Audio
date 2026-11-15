
/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    headphonetopo.h

Abstract:

    Declaration of topology miniport for the headphone (internal).
--*/

#ifndef _SIMPLEAUDIOSAMPLE_HEADPHONETOPO_H_
#define _SIMPLEAUDIOSAMPLE_HEADPHONETOPO_H_

NTSTATUS PropertyHandler_HeadphoneTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);

NTSTATUS PropertyHandler_HeadphoneTopology(_In_ PPCPROPERTY_REQUEST PropertyRequest);

// cbsof: jack detection
NTSTATUS EventHandler_HeadphoneJack(_In_ PPCEVENT_REQUEST EventRequest);
VOID HeadphoneJackChanged(_In_ PVOID Context, _In_ BOOLEAN Connected);

#endif // _SIMPLEAUDIOSAMPLE_HEADPHONETOPO_H_
