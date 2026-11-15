/*++

cbsof: capture wave filters (built-in DMIC, headset mic).

Derived from Microsoft SimpleAudioSample micarraywavtable.h (MS-PL). Each
endpoint has exactly the format its SOF capture PCM delivers:
  built-in mics  SOF PCM 99 (DMIC0, PDM0 = two mics)     2 ch, 48 kHz, 16-bit
  headset mic    SOF PCM 1 capture (DA7219 on SSP2)      2 ch, 48 kHz, 16-bit
--*/

#ifndef _CBSOF_CAPWAVTABLE_H_
#define _CBSOF_CAPWAVTABLE_H_

#define CBSOF_CAPTURE_RATE          48000
#define CBSOF_CAPTURE_BITS          16
#define DMIC_CHANNELS               2
#define MICIN_CHANNELS              2
#define CBSOF_CAPTURE_MAX_STREAMS   1

// One capture wave filter: bridge pin from the topology filter + host pin.
#define CBSOF_CAPTURE_WAVE(N, CH, MASK)                                                         \
static KSDATAFORMAT_WAVEFORMATEXTENSIBLE N##PinSupportedDeviceFormats[] =                     \
{                                                                                               \
    {                                                                                           \
        {                                                                                       \
            sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, 0, 0,                                 \
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                              \
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),                                             \
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)                                   \
        },                                                                                      \
        {                                                                                       \
            {                                                                                   \
                WAVE_FORMAT_EXTENSIBLE,                                                         \
                CH,                                                                             \
                CBSOF_CAPTURE_RATE,                                                             \
                CBSOF_CAPTURE_RATE * CH * (CBSOF_CAPTURE_BITS / 8),                             \
                CH * (CBSOF_CAPTURE_BITS / 8),                                                  \
                CBSOF_CAPTURE_BITS,                                                             \
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)                             \
            },                                                                                  \
            CBSOF_CAPTURE_BITS,                                                                 \
            MASK,                                                                               \
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM)                                              \
        }                                                                                       \
    }                                                                                           \
};                                                                                              \
static MODE_AND_DEFAULT_FORMAT N##PinSupportedDeviceModes[] =                                  \
{                                                                                               \
    { STATIC_AUDIO_SIGNALPROCESSINGMODE_RAW, &N##PinSupportedDeviceFormats[0].DataFormat }      \
};                                                                                              \
static PIN_DEVICE_FORMATS_AND_MODES N##PinDeviceFormatsAndModes[] =                            \
{                                                                                               \
    { BridgePin, NULL, 0, NULL, 0 },                                                            \
    { SystemCapturePin, N##PinSupportedDeviceFormats, SIZEOF_ARRAY(N##PinSupportedDeviceFormats), \
      N##PinSupportedDeviceModes, SIZEOF_ARRAY(N##PinSupportedDeviceModes) }                    \
};                                                                                              \
static KSDATARANGE_AUDIO N##PinDataRangesStream[] =                                            \
{                                                                                               \
    {                                                                                           \
        {                                                                                       \
            sizeof(KSDATARANGE_AUDIO), KSDATARANGE_ATTRIBUTES, 0, 0,                            \
            STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                              \
            STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),                                             \
            STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)                                   \
        },                                                                                      \
        CH, CBSOF_CAPTURE_BITS, CBSOF_CAPTURE_BITS, CBSOF_CAPTURE_RATE, CBSOF_CAPTURE_RATE      \
    },                                                                                          \
};                                                                                              \
static PKSDATARANGE N##PinDataRangePointersStream[] =                                          \
{                                                                                               \
    PKSDATARANGE(&N##PinDataRangesStream[0]),                                                   \
    PKSDATARANGE(&PinDataRangeAttributeList),                                                   \
};                                                                                              \
static KSDATARANGE N##PinDataRangesBridge[] =                                                  \
{                                                                                               \
    {                                                                                           \
        sizeof(KSDATARANGE), 0, 0, 0,                                                           \
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),                                                  \
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),                                              \
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)                                               \
    }                                                                                           \
};                                                                                              \
static PKSDATARANGE N##PinDataRangePointersBridge[] = { &N##PinDataRangesBridge[0] };          \
static PCPIN_DESCRIPTOR N##WaveMiniportPins[] =                                                \
{                                                                                               \
    /* KSPIN_WAVE_BRIDGE */                                                                     \
    {                                                                                           \
        0, 0, 0, NULL,                                                                          \
        { 0, NULL, 0, NULL, SIZEOF_ARRAY(N##PinDataRangePointersBridge), N##PinDataRangePointersBridge, \
          KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 }             \
    },                                                                                          \
    /* KSPIN_WAVEIN_HOST */                                                                     \
    {                                                                                           \
        CBSOF_CAPTURE_MAX_STREAMS, CBSOF_CAPTURE_MAX_STREAMS, 0, NULL,                          \
        { 0, NULL, 0, NULL, SIZEOF_ARRAY(N##PinDataRangePointersStream), N##PinDataRangePointersStream, \
          KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SINK, &KSCATEGORY_AUDIO,                      \
          &KSAUDFNAME_RECORDING_CONTROL, 0 }                                                    \
    }                                                                                           \
};                                                                                              \
static PCNODE_DESCRIPTOR N##WaveMiniportNodes[] =                                              \
{                                                                                               \
    { 0, NULL, &KSNODETYPE_ADC, NULL }      /* KSNODE_WAVE_ADC */                               \
};                                                                                              \
static PCCONNECTION_DESCRIPTOR N##WaveMiniportConnections[] =                                  \
{                                                                                               \
    { PCFILTER_NODE,    KSPIN_WAVE_BRIDGE,  KSNODE_WAVE_ADC,    1 },                            \
    { KSNODE_WAVE_ADC,  0,                  PCFILTER_NODE,      KSPIN_WAVEIN_HOST },            \
};                                                                                              \
static SIMPLEAUDIOSAMPLE_PROPERTY_ITEM N##PropertiesWaveFilter[] =                             \
{                                                                                               \
    { { &KSPROPSETID_General, KSPROPERTY_GENERAL_COMPONENTID,                                   \
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyHandler_WaveFilter },       \
      0, 0, NULL, NULL, NULL, NULL, 0 },                                                        \
    { { &KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT,                                     \
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyHandler_WaveFilter },       \
      0, 0, NULL, NULL, NULL, NULL, 0 },                                                        \
    { { &KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT2,                                    \
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, PropertyHandler_WaveFilter },       \
      0, 0, NULL, NULL, NULL, NULL, 0 },                                                        \
};                                                                                              \
DEFINE_PCAUTOMATION_TABLE_PROP(Automation##N##WaveFilter, N##PropertiesWaveFilter);            \
static PCFILTER_DESCRIPTOR N##WaveMiniportFilterDescriptor =                                   \
{                                                                                               \
    0, &Automation##N##WaveFilter,                                                              \
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(N##WaveMiniportPins), N##WaveMiniportPins,           \
    sizeof(PCNODE_DESCRIPTOR), SIZEOF_ARRAY(N##WaveMiniportNodes), N##WaveMiniportNodes,        \
    SIZEOF_ARRAY(N##WaveMiniportConnections), N##WaveMiniportConnections,                       \
    0, NULL                                                                                     \
};

// Built-in mics: the two mics on PDM0 as left/right.
CBSOF_CAPTURE_WAVE(Dmic, DMIC_CHANNELS, KSAUDIO_SPEAKER_STEREO)
// Headset mic: the DA7219 sends the mic on the left I2S slot.
CBSOF_CAPTURE_WAVE(MicIn, MICIN_CHANNELS, KSAUDIO_SPEAKER_STEREO)

#endif // _CBSOF_CAPWAVTABLE_H_
