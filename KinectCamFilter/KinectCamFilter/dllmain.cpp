#include <streams.h>
#include <initguid.h>

DEFINE_GUID(CLSID_KinectVirtualCam, 0x8b349272, 0xbdd5, 0x42cb, 0x90, 0xfa, 0x98, 0xd1, 0x66, 0xf3, 0xcc, 0x3);

class CKinectStream : public CSourceStream, public IAMStreamConfig, public IKsPropertySet {
public:
    CKinectStream(HRESULT* phr, CSource* pParent, LPCWSTR pPinName);
    ~CKinectStream();
    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv);
    HRESULT FillBuffer(IMediaSample* pms) override;
    HRESULT DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pProperties) override;
    HRESULT CheckMediaType(const CMediaType* pMediaType) override;
    HRESULT GetMediaType(int iPosition, CMediaType* pmt) override;
    HRESULT SetMediaType(const CMediaType* pMediaType) override;
    HRESULT OnThreadStartPlay() override;
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE* pmt) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE** ppmt) override { *ppmt = CreateMediaType(&m_mt); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int* piCount, int* piSize) override;
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int iIndex, AM_MEDIA_TYPE** pmt, BYTE* pSCC) override;
    HRESULT STDMETHODCALLTYPE Set(REFGUID guidPropSet, DWORD dwID, void* pInstanceData, DWORD cbInstanceData, void* pPropData, DWORD cbPropData) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Get(REFGUID guidPropSet, DWORD dwPropID, void* pInstanceData, DWORD cbInstanceData, void* pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    HRESULT STDMETHODCALLTYPE QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;
private:
    HANDLE m_hSharedMemory;
    void* m_pSharedBuffer;
    REFERENCE_TIME m_rtStreamTime;
};

class CKinectVirtualCam : public CSource {
public: static CUnknown* WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT* phr);
private: CKinectVirtualCam(LPUNKNOWN lpunk, HRESULT* phr);
};

CKinectVirtualCam::CKinectVirtualCam(LPUNKNOWN lpunk, HRESULT* phr) : CSource(NAME("Kinect Virtual Cam"), lpunk, CLSID_KinectVirtualCam) { new CKinectStream(phr, this, L"Video Out"); }
CUnknown* WINAPI CKinectVirtualCam::CreateInstance(LPUNKNOWN lpunk, HRESULT* phr) { return new CKinectVirtualCam(lpunk, phr); }

CKinectStream::CKinectStream(HRESULT* phr, CSource* pParent, LPCWSTR pPinName) : CSourceStream(NAME("Video Out"), phr, pParent, pPinName), m_hSharedMemory(NULL), m_pSharedBuffer(NULL), m_rtStreamTime(0) {
    GetMediaType(0, &m_mt);
    m_hSharedMemory = OpenFileMappingA(FILE_MAP_READ, FALSE, "KinectSharedMem");
    if (m_hSharedMemory) m_pSharedBuffer = MapViewOfFile(m_hSharedMemory, FILE_MAP_READ, 0, 0, 0);
}
CKinectStream::~CKinectStream() { if (m_pSharedBuffer) UnmapViewOfFile(m_pSharedBuffer); if (m_hSharedMemory) CloseHandle(m_hSharedMemory); }

STDMETHODIMP CKinectStream::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IAMStreamConfig) return GetInterface((IAMStreamConfig*)this, ppv);
    else if (riid == IID_IKsPropertySet) return GetInterface((IKsPropertySet*)this, ppv);
    return CSourceStream::NonDelegatingQueryInterface(riid, ppv);
}

HRESULT CKinectStream::CheckMediaType(const CMediaType* pmt) {
    if (!pmt) return E_POINTER;
    if (*pmt->Type() != MEDIATYPE_Video || *pmt->Subtype() != MEDIASUBTYPE_RGB32 || *pmt->FormatType() != FORMAT_VideoInfo)
        return VFW_E_TYPE_NOT_ACCEPTED;

    VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)pmt->Format();
    if (pvi->bmiHeader.biWidth == 640 && abs(pvi->bmiHeader.biHeight) == 480) {
        return S_OK;
    }
    return VFW_E_TYPE_NOT_ACCEPTED;
}

HRESULT CKinectStream::GetMediaType(int iPosition, CMediaType* pmt) {
    if (iPosition != 0) return E_INVALIDARG;

    pmt->SetType(&MEDIATYPE_Video);
    pmt->SetSubtype(&MEDIASUBTYPE_RGB32);
    pmt->SetFormatType(&FORMAT_VideoInfo);

    VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER));
    ZeroMemory(pvi, sizeof(VIDEOINFOHEADER));
    pvi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pvi->bmiHeader.biPlanes = 1;
    pvi->bmiHeader.biBitCount = 32;
    pvi->bmiHeader.biWidth = 640;
    pvi->bmiHeader.biHeight = 480;
    pvi->AvgTimePerFrame = 333333; // 30 FPS
    pvi->bmiHeader.biSizeImage = 640 * 480 * 4;

    return S_OK;
}

HRESULT CKinectStream::SetMediaType(const CMediaType* pMediaType) { return S_OK; }

HRESULT STDMETHODCALLTYPE CKinectStream::GetNumberOfCapabilities(int* piCount, int* piSize) {
    *piCount = 1;
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CKinectStream::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** pmt, BYTE* pSCC) {
    if (iIndex != 0) return E_INVALIDARG;

    *pmt = CreateMediaType(&m_mt);
    GetMediaType(0, (CMediaType*)*pmt);

    VIDEO_STREAM_CONFIG_CAPS* pvscc = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
    ZeroMemory(pvscc, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    pvscc->guid = FORMAT_VideoInfo;
    pvscc->VideoStandard = AnalogVideo_None;

    pvscc->InputSize.cx = 640; pvscc->InputSize.cy = 480;
    pvscc->MinCroppingSize.cx = 640; pvscc->MinCroppingSize.cy = 480;
    pvscc->MaxCroppingSize.cx = 640; pvscc->MaxCroppingSize.cy = 480;
    pvscc->MinOutputSize.cx = 640; pvscc->MinOutputSize.cy = 480;
    pvscc->MaxOutputSize.cx = 640; pvscc->MaxOutputSize.cy = 480;
    pvscc->MinFrameInterval = 333333; pvscc->MaxFrameInterval = 333333;
    pvscc->MinBitsPerSecond = 640 * 480 * 32 * 30;
    pvscc->MaxBitsPerSecond = 640 * 480 * 32 * 30;
    pvscc->CropGranularityX = 1; pvscc->CropGranularityY = 1;
    pvscc->OutputGranularityX = 1; pvscc->OutputGranularityY = 1;

    return S_OK;
}

HRESULT STDMETHODCALLTYPE CKinectStream::Get(REFGUID guidPropSet, DWORD dwPropID, void* pInstanceData, DWORD cbInstanceData, void* pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet != AMPROPSETID_Pin || dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_SET_UNSUPPORTED;
    if (pPropData && cbPropData >= sizeof(GUID)) { *(GUID*)pPropData = PIN_CATEGORY_CAPTURE; if (pcbReturned) *pcbReturned = sizeof(GUID); return S_OK; } return E_INVALIDARG;
}
HRESULT STDMETHODCALLTYPE CKinectStream::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet != AMPROPSETID_Pin || dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_SET_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET; return S_OK;
}

HRESULT CKinectStream::DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pProperties) {
    CAutoLock filterLock(m_pFilter->pStateLock());
    VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)m_mt.Format();
    pProperties->cBuffers = 1;
    pProperties->cbBuffer = pvi->bmiHeader.biSizeImage;
    ALLOCATOR_PROPERTIES Actual;
    HRESULT hr = pAlloc->SetProperties(pProperties, &Actual);
    if (FAILED(hr) || Actual.cbBuffer < pProperties->cbBuffer) return E_FAIL;
    return S_OK;
}

HRESULT CKinectStream::OnThreadStartPlay() { m_rtStreamTime = 0; return CSourceStream::OnThreadStartPlay(); }

HRESULT CKinectStream::FillBuffer(IMediaSample* pms) {
    // --- EL FRENO DE MANO (THROTTLE DE 30 FPS) ---
    // Esto evita que la CPU se derrita y arregla el colapso de WebRTC
    static ULONGLONG lastTick = GetTickCount64();
    ULONGLONG currentTick = GetTickCount64();
    ULONGLONG elapsed = currentTick - lastTick;

    // Si han pasado menos de 33ms desde el último frame, dormimos el hilo
    if (elapsed < 33) {
        Sleep((DWORD)(33 - elapsed));
    }
    lastTick = GetTickCount64();
    // ---------------------------------------------

    BYTE* pData;
    pms->GetPointer(&pData);

    VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)m_mt.Format();
    REFERENCE_TIME rtStart = m_rtStreamTime;
    m_rtStreamTime += pvi->AvgTimePerFrame;
    REFERENCE_TIME rtStop = m_rtStreamTime;
    pms->SetTime(&rtStart, &rtStop);
    pms->SetSyncPoint(TRUE);

    if (m_pSharedBuffer) {
        int inWidth = *((int*)m_pSharedBuffer);
        int inHeight = *(((int*)m_pSharedBuffer) + 1);
        BYTE* inPixels = (BYTE*)m_pSharedBuffer + 8;

        if (inWidth == 640 && inHeight == 480) {
            memcpy(pData, inPixels, 640 * 480 * 4);
        }
        else {
            long lDataLen = pms->GetSize();
            for (long i = 0; i < lDataLen; i += 4) {
                pData[i] = 0; pData[i + 1] = 0; pData[i + 2] = 0; pData[i + 3] = 255;
            }
        }
    }
    return S_OK;
}

const REGPINTYPES sudPinTypes = { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB32 };
const REGFILTERPINS sudpPins = { (LPWSTR)L"Output", FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, NULL, 1, &sudPinTypes };
const AMOVIESETUP_FILTER sudFilter = { &CLSID_KinectVirtualCam, (LPWSTR)L"Kinect Virtual Cam", MERIT_DO_NOT_USE, 1, &sudpPins };
CFactoryTemplate g_Templates[] = { { (LPWSTR)L"Kinect Virtual Cam", &CLSID_KinectVirtualCam, CKinectVirtualCam::CreateInstance, NULL, &sudFilter } };
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

STDAPI DllRegisterServer() {
    HRESULT hr = AMovieDllRegisterServer2(TRUE);
    if (FAILED(hr)) return hr;
    IFilterMapper2* pFM2 = NULL;
    hr = CoCreateInstance(CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&pFM2);
    if (SUCCEEDED(hr)) {
        IMoniker* pMoniker = NULL;
        REGFILTER2 rf2; rf2.dwVersion = 1; rf2.cPins = 1; rf2.rgPins = &sudpPins;
        hr = pFM2->RegisterFilter(CLSID_KinectVirtualCam, L"Kinect Virtual Cam", &pMoniker, &CLSID_VideoInputDeviceCategory, NULL, &rf2);
        if (pMoniker) pMoniker->Release();
        pFM2->Release();
    }
    return hr;
}

STDAPI DllUnregisterServer() {
    HRESULT hr = AMovieDllRegisterServer2(FALSE);
    if (FAILED(hr)) return hr;
    IFilterMapper2* pFM2 = NULL;
    hr = CoCreateInstance(CLSID_FilterMapper2, NULL, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&pFM2);
    if (SUCCEEDED(hr)) {
        hr = pFM2->UnregisterFilter(&CLSID_VideoInputDeviceCategory, NULL, CLSID_KinectVirtualCam);
        pFM2->Release();
    }
    return hr;
}

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL APIENTRY DllMain(HANDLE hModule, DWORD dwReason, LPVOID lpReserved) { return DllEntryPoint((HINSTANCE)(hModule), dwReason, lpReserved); }