/*++

cbsof: capture topology filters (built-in DMIC, headset mic).

Derived from Microsoft SimpleAudioSample micarray1toptable.h (MS-PL). The
mic elements pin carries the jack description and the jack-change event (on
the filter automation table, as in SysVAD; Windows never subscribes to a pin
table). No volume/mute nodes: Windows applies the level in software.
--*/

#ifndef _CBSOF_CAPTOPTABLE_H_
#define _CBSOF_CAPTOPTABLE_H_

#define CBSOF_CAPTURE_TOPO(N, CATEGORY)                                                         \
static KSDATARANGE N##TopoPinDataRangesBridge[] =                                              \
{                                                                                               \
    {                                                                                           \
        sizeof(KSDATARANGE), 0, 0, 0,                                                           \
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                                  \
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),                                              \
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)                                               \
    }                                                                                           \
};                                                                                              \
static PKSDATARANGE N##TopoPinDataRangePointersBridge[] = { &N##TopoPinDataRangesBridge[0] };  \
static PCPIN_DESCRIPTOR N##TopoMiniportPins[] =                                                \
{                                                                                               \
    /* KSPIN_TOPO_MIC_ELEMENTS */                                                               \
    {                                                                                           \
        0, 0, 0, NULL,                                                                          \
        { 0, NULL, 0, NULL, SIZEOF_ARRAY(N##TopoPinDataRangePointersBridge), N##TopoPinDataRangePointersBridge, \
          KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, CATEGORY, NULL, 0 }                      \
    },                                                                                          \
    /* KSPIN_TOPO_BRIDGE */                                                                     \
    {                                                                                           \
        0, 0, 0, NULL,                                                                          \
        { 0, NULL, 0, NULL, SIZEOF_ARRAY(N##TopoPinDataRangePointersBridge), N##TopoPinDataRangePointersBridge, \
          KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 }            \
    }                                                                                           \
};                                                                                              \
static PCCONNECTION_DESCRIPTOR N##TopoMiniportConnections[] =                                  \
{                                                                                               \
    { PCFILTER_NODE, KSPIN_TOPO_MIC_ELEMENTS, PCFILTER_NODE, KSPIN_TOPO_BRIDGE }                \
};                                                                                              \
static PCPROPERTY_ITEM N##PropertiesTopoFilter[] =                                             \
{                                                                                               \
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION,                                           \
      KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyHandler_##N##TopoFilter },    \
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2,                                          \
      KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyHandler_##N##TopoFilter },    \
};                                                                                              \
static PCEVENT_ITEM N##JackEvents[] =                                                          \
{                                                                                               \
    { &KSEVENTSETID_PinCapsChange, KSEVENT_PINCAPS_JACKINFOCHANGE,                              \
      KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT, EventHandler_##N##Jack }                 \
};                                                                                              \
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(Automation##N##TopoFilter, N##PropertiesTopoFilter, N##JackEvents); \
static PCFILTER_DESCRIPTOR N##TopoMiniportFilterDescriptor =                                   \
{                                                                                               \
    0, &Automation##N##TopoFilter,                                                              \
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(N##TopoMiniportPins), N##TopoMiniportPins,           \
    sizeof(PCNODE_DESCRIPTOR), 0, NULL,                                                         \
    SIZEOF_ARRAY(N##TopoMiniportConnections), N##TopoMiniportConnections,                       \
    0, NULL                                                                                     \
};

CBSOF_CAPTURE_TOPO(Dmic, &KSNODETYPE_MICROPHONE)
CBSOF_CAPTURE_TOPO(MicIn, &KSNODETYPE_HEADSET_MICROPHONE)

// Jack descriptions (IsConnected is filled in from sofjack.cpp on every query).
// Only the mic elements pin has one.
static KSJACK_DESCRIPTION DmicJackDesc =
{
    KSAUDIO_SPEAKER_QUAD,
    0,
    eConnTypeUnknown,
    eGeoLocFront,
    eGenLocPrimaryBox,
    ePortConnIntegratedDevice,
    TRUE
};
static PKSJACK_DESCRIPTION DmicJackDescriptions[] = { &DmicJackDesc, NULL };

static KSJACK_DESCRIPTION MicInJackDesc =
{
    KSAUDIO_SPEAKER_STEREO,
    JACKDESC_RGB(0x00,0x00,0x00),               // the black 3.5 mm combo jack
    eConnType3Point5mm,
    eGeoLocLeft,
    eGenLocPrimaryBox,
    ePortConnJack,
    FALSE
};
static PKSJACK_DESCRIPTION MicInJackDescriptions[] = { &MicInJackDesc, NULL };

#endif // _CBSOF_CAPTOPTABLE_H_
