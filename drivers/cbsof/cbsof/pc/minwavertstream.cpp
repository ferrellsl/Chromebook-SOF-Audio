/*++
cbsof WaveRT stream on a SOF pipeline.

Structure follows Microsoft's SimpleAudioSample (MS-PL); the DMA simulation
is replaced by the HDA host DMA + SOF firmware stream in ..\sofpcm.cpp.
--*/
#pragma warning (disable : 4127)

#include "definitions.h"
#include "endpoints.h"
#include "minwavert.h"
#include "minwavertstream.h"
#define MINWAVERTSTREAM_POOLTAG 'SRWM'

#define CBSOF_MAX_BUFFER_BYTES  (256 * PAGE_SIZE)   // HDA BDL entries, one per page

//=============================================================================
#pragma code_seg("PAGE")
CMiniportWaveRTStream::~CMiniportWaveRTStream(void)
{
    PAGED_CODE();
    if (m_bPcmOpen) {
        SofPcmClose(&m_Pcm);
        m_bPcmOpen = FALSE;
    }
    if (m_pMiniport) {
        if (m_bUnregisterStream) {
            m_pMiniport->StreamClosed(m_ulPin, this);
            m_bUnregisterStream = FALSE;
        }
        m_pMiniport->Release();
        m_pMiniport = NULL;
    }
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRTStream::Init
(
    _In_ PCMiniportWaveRT           Miniport_,
    _In_ PPORTWAVERTSTREAM          PortStream_,
    _In_ ULONG                      Pin_,
    _In_ BOOLEAN                    Capture_,
    _In_ PKSDATAFORMAT              DataFormat_,
    _In_ GUID                       SignalProcessingMode
)
{
    PAGED_CODE();

    m_pMiniport = NULL;
    m_pPortStream = PortStream_;
    m_ulPin = Pin_;
    m_bCapture = Capture_;
    m_bUnregisterStream = FALSE;
    m_KsState = KSSTATE_STOP;
    m_ulContentId = 0;
    m_SignalProcessingMode = SignalProcessingMode;
    m_pMdl = NULL;
    m_ulBufferSize = 0;
    m_bPcmOpen = FALSE;
    RtlZeroMemory(&m_Pcm, sizeof(m_Pcm));

    PWAVEFORMATEX pWfEx = GetWaveFormatEx(DataFormat_);
    if (pWfEx == NULL) {
        return STATUS_UNSUCCESSFUL;
    }
    m_ulChannels = pWfEx->nChannels;
    m_ulSampleRate = pWfEx->nSamplesPerSec;
    m_ulBitsPerSample = pWfEx->wBitsPerSample;

    m_pMiniport = reinterpret_cast<CMiniportWaveRT*>(Miniport_);
    if (m_pMiniport == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    m_pMiniport->AddRef();

    m_ulPcmId = m_bCapture ? SOF_PCM_DMIC : SOF_PCM_SPEAKERS;

    NTSTATUS ntStatus = m_pMiniport->StreamCreated(m_ulPin, this);
    if (NT_SUCCESS(ntStatus)) {
        m_bUnregisterStream = TRUE;
    }
    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::NonDelegatingQueryInterface
(
    _In_ REFIID  Interface,
    _COM_Outptr_ PVOID * Object
)
{
    PAGED_CODE();
    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IDrmAudioStream)) {
        *Object = (PVOID)(IDrmAudioStream*)this;
    } else {
        *Object = NULL;
    }

    if (*Object) {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::AllocateAudioBuffer
(
    _In_    ULONG                   RequestedSize_,
    _Out_   PMDL                   *AudioBufferMdl_,
    _Out_   ULONG                  *ActualSize_,
    _Out_   ULONG                  *OffsetFromFirstPage_,
    _Out_   MEMORY_CACHING_TYPE    *CacheType_
)
{
    PAGED_CODE();

    if (m_pMdl) {
        return STATUS_UNSUCCESSFUL;
    }
    if (RequestedSize_ == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    // Whole pages, so every BDL entry covers exactly one page.
    ULONG size = (RequestedSize_ + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (size > CBSOF_MAX_BUFFER_BYTES) {
        size = CBSOF_MAX_BUFFER_BYTES;
    }

    PHYSICAL_ADDRESS high;
    high.QuadPart = 0xFFFFFFFF;     // SOF page table entries are 20-bit PFNs
    PMDL mdl = m_pPortStream->AllocatePagesForMdl(high, size);
    if (mdl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (MmGetMdlByteCount(mdl) < size) {
        m_pPortStream->FreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_pMdl = mdl;
    m_ulBufferSize = size;
    *AudioBufferMdl_ = mdl;
    *ActualSize_ = size;
    *OffsetFromFirstPage_ = 0;
    *CacheType_ = MmNonCached;
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
VOID CMiniportWaveRTStream::FreeAudioBuffer
(
    _In_opt_    PMDL        Mdl_,
    _In_        ULONG       Size_
)
{
    UNREFERENCED_PARAMETER(Size_);
    PAGED_CODE();

    if (m_bPcmOpen) {
        SofPcmClose(&m_Pcm);
        m_bPcmOpen = FALSE;
    }
    if (Mdl_ != NULL) {
        m_pPortStream->FreePagesFromMdl(Mdl_);
    }
    m_pMdl = NULL;
    m_ulBufferSize = 0;
}

//=============================================================================
#pragma code_seg("PAGE")
VOID CMiniportWaveRTStream::GetHWLatency
(
    _Out_   KSRTAUDIO_HWLATENCY     *hwLatency_
)
{
    PAGED_CODE();
    ASSERT(hwLatency_);
    hwLatency_->ChipsetDelay = 0;
    hwLatency_->CodecDelay = 0;
    hwLatency_->FifoSize = 2 * 768;     // the two 768-byte DSP pipeline buffers
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::GetPositionRegister
(
    _Out_   KSRTAUDIO_HWREGISTER    *Register_
)
{
    UNREFERENCED_PARAMETER(Register_);
    PAGED_CODE();
    return STATUS_NOT_SUPPORTED;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::GetClockRegister
(
    _Out_   KSRTAUDIO_HWREGISTER    *Register_
)
{
    UNREFERENCED_PARAMETER(Register_);
    PAGED_CODE();
    return STATUS_NOT_SUPPORTED;
}

//=============================================================================
#pragma code_seg()
NTSTATUS CMiniportWaveRTStream::GetPosition
(
    _Out_   KSAUDIO_POSITION    *Position_
)
{
    ASSERT(Position_);
    ULONG pos = m_bPcmOpen ? SofPcmPosition(&m_Pcm) : 0;
    Position_->PlayOffset = pos;
    Position_->WriteOffset = pos;
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::SetState
(
    _In_    KSSTATE State_
)
{
    PAGED_CODE();
    NTSTATUS ntStatus = STATUS_SUCCESS;

    switch (State_)
    {
    case KSSTATE_STOP:
        if (m_bPcmOpen) {
            SofPcmClose(&m_Pcm);
            m_bPcmOpen = FALSE;
        }
        break;

    case KSSTATE_ACQUIRE:
        if (m_KsState == KSSTATE_STOP && !m_bPcmOpen) {
            ntStatus = SofPcmOpen(&m_Pcm, m_ulPcmId, m_bCapture, m_pMdl, m_ulBufferSize,
                                  m_ulChannels, m_ulSampleRate, m_ulBitsPerSample);
            m_bPcmOpen = NT_SUCCESS(ntStatus);
        }
        break;

    case KSSTATE_PAUSE:
        if (m_KsState == KSSTATE_RUN && m_bPcmOpen) {
            ntStatus = SofPcmPause(&m_Pcm);
        }
        break;

    case KSSTATE_RUN:
        if (!m_bPcmOpen) {
            ntStatus = SofPcmOpen(&m_Pcm, m_ulPcmId, m_bCapture, m_pMdl, m_ulBufferSize,
                                  m_ulChannels, m_ulSampleRate, m_ulBitsPerSample);
            m_bPcmOpen = NT_SUCCESS(ntStatus);
        }
        if (m_bPcmOpen) {
            ntStatus = SofPcmRun(&m_Pcm);
        }
        break;
    }

    if (NT_SUCCESS(ntStatus)) {
        m_KsState = State_;
    }
    return ntStatus;
}

//=============================================================================
#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::SetFormat
(
    _In_    KSDATAFORMAT    *DataFormat_
)
{
    PAGED_CODE();
    PWAVEFORMATEX pWfEx = GetWaveFormatEx(DataFormat_);
    if (pWfEx == NULL || m_KsState != KSSTATE_STOP) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (pWfEx->nChannels != m_ulChannels || pWfEx->nSamplesPerSec != m_ulSampleRate ||
        pWfEx->wBitsPerSample != m_ulBitsPerSample) {
        return STATUS_NOT_SUPPORTED;
    }
    return STATUS_SUCCESS;
}

//=============================================================================
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetContentId
(
    _In_  ULONG       contentId,
    _In_  PCDRMRIGHTS drmRights
)
{
    UNREFERENCED_PARAMETER(drmRights);
    PAGED_CODE();
    m_ulContentId = contentId;
    return STATUS_SUCCESS;
}
#pragma code_seg()
