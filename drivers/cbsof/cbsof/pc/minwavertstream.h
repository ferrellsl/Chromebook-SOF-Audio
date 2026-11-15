/*++
cbsof WaveRT stream: a real hardware stream through a SOF pipeline.

Replaces the simulated stream of Microsoft's SimpleAudioSample (MS-PL, see
LICENSE-MS-PL.txt); the class name and Init() signature are kept so the
sample's CMiniportWaveRT can create it unchanged.
--*/
#ifndef _CBSOF_MINWAVERTSTREAM_H_
#define _CBSOF_MINWAVERTSTREAM_H_

#include "..\sofapi.h"

class CMiniportWaveRT;
typedef CMiniportWaveRT *PCMiniportWaveRT;

class CMiniportWaveRTStream :
    public IDrmAudioStream,
    public IMiniportWaveRTStream,
    public CUnknown
{
protected:
    PPORTWAVERTSTREAM   m_pPortStream;

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRTStream);
    ~CMiniportWaveRTStream();

    IMP_IMiniportWaveRTStream;
    IMP_IDrmAudioStream;

    NTSTATUS Init
    (
        _In_  PCMiniportWaveRT    Miniport,
        _In_  PPORTWAVERTSTREAM   Stream,
        _In_  ULONG               Channel,
        _In_  BOOLEAN             Capture,
        _In_  PKSDATAFORMAT       DataFormat,
        _In_  GUID                SignalProcessingMode
    );

    friend class CMiniportWaveRT;

protected:
    CMiniportWaveRT*    m_pMiniport;
    ULONG               m_ulPin;
    BOOLEAN             m_bCapture;
    BOOLEAN             m_bUnregisterStream;
    KSSTATE             m_KsState;
    ULONG               m_ulContentId;
    GUID                m_SignalProcessingMode;

    PMDL                m_pMdl;
    ULONG               m_ulBufferSize;
    ULONG               m_ulChannels;
    ULONG               m_ulSampleRate;
    ULONG               m_ulBitsPerSample;
    ULONG               m_ulPcmId;

    SOF_PCM             m_Pcm;
    BOOLEAN             m_bPcmOpen;

public:
    GUID GetSignalProcessingMode() { return m_SignalProcessingMode; }
};
typedef CMiniportWaveRTStream *PCMiniportWaveRTStream;

#endif // _CBSOF_MINWAVERTSTREAM_H_
