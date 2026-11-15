/*++

cbsof: capture topology handlers (built-in DMIC, headset mic).
--*/

#ifndef _CBSOF_CAPTOPO_H_
#define _CBSOF_CAPTOPO_H_

NTSTATUS PropertyHandler_DmicTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS PropertyHandler_MicInTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS EventHandler_DmicJack(_In_ PPCEVENT_REQUEST EventRequest);
NTSTATUS EventHandler_MicInJack(_In_ PPCEVENT_REQUEST EventRequest);
VOID DmicJackChanged(_In_ PVOID Context, _In_ BOOLEAN Connected);
VOID MicInJackChanged(_In_ PVOID Context, _In_ BOOLEAN Connected);

#endif // _CBSOF_CAPTOPO_H_
