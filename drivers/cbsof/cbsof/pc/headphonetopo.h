
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

#endif // _SIMPLEAUDIOSAMPLE_HEADPHONETOPO_H_
