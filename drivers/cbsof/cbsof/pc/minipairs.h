/*++

Copyright (c) Microsoft Corporation All Rights Reserved

Module Name:

    minipairs.h

Abstract:

    Local audio endpoint filter definitions. 
--*/

#ifndef _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_
#define _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_

#include "speakertopo.h"
#include "speakertoptable.h"
#include "speakerwavtable.h"

#include "headphonetopo.h"
#include "headphonetoptable.h"

#include "captopo.h"
#include "captoptable.h"
#include "capwavtable.h"


NTSTATUS
CreateMiniportWaveRTSimpleAudioSample
( 
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

NTSTATUS
CreateMiniportTopologySimpleAudioSample
( 
    _Out_       PUNKNOWN *,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN,
    _In_        POOL_FLAGS,
    _In_        PUNKNOWN,
    _In_opt_    PVOID,
    _In_        PENDPOINT_MINIPAIR
);

//
// Render miniports.
//

/*********************************************************************
* Topology/Wave bridge connection for speaker (internal)             *
*                                                                    *
*              +------+                +------+                      *
*              | Wave |                | Topo |                      *
*              |      |                |      |                      *
* System   --->|0    1|--------------->|0    1|---> Line Out         *
*              |      |                |      |                      *
*              +------+                +------+                      *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE SpeakerTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_WAVEOUT_SOURCE,  // TopologyIn
        KSPIN_WAVE_RENDER3_SOURCE,   // WaveOut
        CONNECTIONTYPE_WAVE_OUTPUT
    }
};

static
ENDPOINT_MINIPAIR SpeakerMiniports =
{
    eSpeakerDevice,
    L"TopologySpeaker",                                     // make sure this or the template name matches with KSNAME_TopologySpeaker in the inf's [Strings] section 
    NULL,                                                   // optional template name
    CreateMiniportTopologySimpleAudioSample,
    &SpeakerTopoMiniportFilterDescriptor,
    0, NULL,                                                // Interface properties
    L"WaveSpeaker",                                         // make sure this or the template name matches with KSNAME_WaveSpeaker in the inf's [Strings] section
    NULL,                                                   // optional template name
    CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor,
    0,                                                      // Interface properties
    NULL,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

//
// Capture miniports.
//

/*********************************************************************
* cbsof: capture endpoints. Topo (mic elements -> bridge) feeds Wave  *
* (bridge -> host pin):                                               *
*   Microphone          built-in DMIC, SOF PCM 99, 2 ch (PDM0)       *
*   Headset Microphone  DA7219 on SSP2, SOF PCM 1 capture, 2 ch      *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE CaptureTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_BRIDGE,          // TopologyOut
        KSPIN_WAVE_BRIDGE,          // WaveIn
        CONNECTIONTYPE_TOPOLOGY_OUTPUT
    }
};

static
ENDPOINT_MINIPAIR DmicMiniports =
{
    eMicArrayDevice1,
    L"TopologyDmic",                        // must match KSNAME_TopologyDmic in the INF (renamed in 0.6.2: fresh endpoint)
    NULL,
    CreateMiniportTopologySimpleAudioSample,
    &DmicTopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveDmic",                            // must match KSNAME_WaveDmic in the INF
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &DmicWaveMiniportFilterDescriptor,
    0,
    NULL,
    DMIC_CHANNELS,
    DmicPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(DmicPinDeviceFormatsAndModes),
    CaptureTopologyPhysicalConnections,
    SIZEOF_ARRAY(CaptureTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static
ENDPOINT_MINIPAIR MicInMiniports =
{
    eMicInDevice,
    L"TopologyMicIn",                       // must match KSNAME_TopologyMicIn in the INF
    NULL,
    CreateMiniportTopologySimpleAudioSample,
    &MicInTopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveMicIn",                           // must match KSNAME_WaveMicIn in the INF
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &MicInWaveMiniportFilterDescriptor,
    0,
    NULL,
    MICIN_CHANNELS,
    MicInPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(MicInPinDeviceFormatsAndModes),
    CaptureTopologyPhysicalConnections,
    SIZEOF_ARRAY(CaptureTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

/*********************************************************************
* cbsof: Headphones (DA7219 on SSP2, SOF PCM 1). Same wave filter as  *
* the speaker (48 kHz / 16-bit / stereo); its own topology filter     *
* for the 3.5 mm jack.                                                *
*********************************************************************/
static
PHYSICALCONNECTIONTABLE HeadphoneTopologyPhysicalConnections[] =
{
    {
        KSPIN_TOPO_WAVEOUT_SOURCE,  // TopologyIn
        KSPIN_WAVE_RENDER3_SOURCE,  // WaveOut
        CONNECTIONTYPE_WAVE_OUTPUT
    }
};

static
ENDPOINT_MINIPAIR HeadphoneMiniports =
{
    eHeadphoneDevice,
    L"TopologyHeadphones",                                  // must match KSNAME_TopologyHeadphones in the INF
    NULL,
    CreateMiniportTopologySimpleAudioSample,
    &HeadphoneTopoMiniportFilterDescriptor,
    0, NULL,
    L"WaveHeadphones",                                      // must match KSNAME_WaveHeadphones in the INF
    NULL,
    CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor,
    0,
    NULL,
    SPEAKER_DEVICE_MAX_CHANNELS,
    SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes),
    HeadphoneTopologyPhysicalConnections,
    SIZEOF_ARRAY(HeadphoneTopologyPhysicalConnections),
    ENDPOINT_NO_FLAGS,
};

static
PENDPOINT_MINIPAIR  g_RenderEndpoints[] =
{
    &SpeakerMiniports,
    &HeadphoneMiniports,
};

#define g_cRenderEndpoints  (SIZEOF_ARRAY(g_RenderEndpoints))

//=============================================================================
//
// Capture miniport pairs. NOTE: the split of render and capture is arbitrary and
// unnessary, this array could contain render endpoints.
//
static
PENDPOINT_MINIPAIR  g_CaptureEndpoints[] =
{
    &DmicMiniports,
    &MicInMiniports,
};

#define g_cCaptureEndpoints (SIZEOF_ARRAY(g_CaptureEndpoints))

//=============================================================================
//
// Total miniports = # endpoints * 2 (topology + wave).
//
#define g_MaxMiniports  ((g_cRenderEndpoints + g_cCaptureEndpoints) * 2)

#endif // _SIMPLEAUDIOSAMPLE_MINIPAIRS_H_
