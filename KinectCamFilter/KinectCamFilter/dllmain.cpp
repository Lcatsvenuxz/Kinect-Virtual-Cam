#include <streams.h>
#include <dvdmedia.h>
#include <initguid.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

DEFINE_GUID(CLSID_KinectVirtualCam, 0x8b349272, 0xbdd5, 0x42cb, 0x90, 0xfa, 0x98, 0xd1, 0x66, 0xf3, 0xcc, 0x3);

// ============================================================================
//  PROTOCOLO IPC (memoria compartida "KinectSharedMem") - VERSION 2
//  Debe coincidir byte a byte con MainWindow.xaml.cs
//
//  Offset  Tam  Campo
//   0      4    magic          'KICT' (0x54434B49)
//   4      4    version        2
//   8      4    width          ancho del frame activo en el sensor (640 o 1280)
//  12      4    height         alto del frame activo en el sensor (480 o 960)
//  16      4    stride         bytes por fila (= width * 4, sin padding)
//  20      4    format         0 = BGRA 32 bpp (Top-Down)
//  24      4    fps            FPS nativos del modo (30 o 12)
//  28      4    active         1 = el controlador esta inyectando, 0 = detenido
//  32      8    sequence       Seqlock: impar = escritura en curso, par = frame estable
//  40      8    timestamp      Marca de tiempo del frame Kinect en unidades de 100 ns
//  48      4    reqWidth       (escribe el filtro) ancho conectado exitosamente en DirectShow
//  52      4    reqHeight      (escribe el filtro) alto conectado exitosamente en DirectShow
//  56      8    reservedBytes  padding hasta 64 bytes
//  64      ..   pixeles BGRA Top-Down, filas contiguas
// ============================================================================
#define KINECT_IPC_MAGIC        0x54434B49u
#define KINECT_IPC_HEADER_SIZE  64
#define KINECT_IPC_MAX_WIDTH    1280
#define KINECT_IPC_MAX_HEIGHT   960
#define KINECT_IPC_TOTAL_SIZE   (KINECT_IPC_HEADER_SIZE + KINECT_IPC_MAX_WIDTH * KINECT_IPC_MAX_HEIGHT * 4)

#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) | ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))
#endif

static const DWORD FOURCC_YUY2 = MAKEFOURCC('Y', 'U', 'Y', '2');

#pragma pack(push, 8)
struct KinectIpcHeader {
    uint32_t magic;
    uint32_t version;
    int32_t  width;
    int32_t  height;
    int32_t  stride;
    int32_t  format;
    int32_t  fps;
    int32_t  active;
    volatile int64_t sequence;
    int64_t  timestamp;
    int32_t  reqWidth;
    int32_t  reqHeight;
    uint8_t  reservedBytes[8];
};
#pragma pack(pop)
static_assert(sizeof(KinectIpcHeader) == KINECT_IPC_HEADER_SIZE, "El encabezado IPC debe medir 64 bytes");

// ----------------------------------------------------------------------------
//  IMPOSICION ESTRICTA DEL SENSOR:
//  El filtro anuncia y acepta UNICAMENTE la resolucion 4:3 activa en IPC
//  (640x480 o 1280x960), aceptando cualquier AvgTimePerFrame (10 a 60 FPS)
//  para máxima compatibilidad con Discord, WebRTC y OBS.
// ----------------------------------------------------------------------------
enum OutputPixFmt {
    PIX_RGB32  = 0,
    PIX_YUY2   = 1,
    PIX_ARGB32 = 2,
    PIX_RGB24  = 3
};

struct StreamConfig {
    int width;
    int height;
    int fps;
    OutputPixFmt pixFmt;
    bool flipRows;
};

static const OutputPixFmt g_pixFmts[] = {
    PIX_RGB32,
    PIX_YUY2,
    PIX_ARGB32,
    PIX_RGB24
};
static const int g_pixFmtCount = sizeof(g_pixFmts) / sizeof(g_pixFmts[0]);

static const GUID* SubtypeFromPixFmt(OutputPixFmt fmt) {
    switch (fmt) {
    case PIX_YUY2:   return &MEDIASUBTYPE_YUY2;
    case PIX_ARGB32: return &MEDIASUBTYPE_ARGB32;
    case PIX_RGB24:  return &MEDIASUBTYPE_RGB24;
    default:         return &MEDIASUBTYPE_RGB32;
    }
}

static WORD BitCountFromPixFmt(OutputPixFmt fmt) {
    switch (fmt) {
    case PIX_YUY2:  return 16;
    case PIX_RGB24: return 24;
    default:        return 32;
    }
}

static DWORD CompressionFromPixFmt(OutputPixFmt fmt) {
    return (fmt == PIX_YUY2) ? FOURCC_YUY2 : BI_RGB;
}

static long CalcStride(int width, OutputPixFmt fmt) {
    switch (fmt) {
    case PIX_YUY2:  return width * 2;
    case PIX_RGB24: return (width * 3 + 3) & ~3;
    default:        return width * 4;
    }
}

static long CalcImageSize(const StreamConfig& cfg) {
    return CalcStride(cfg.width, cfg.pixFmt) * cfg.height;
}

static REFERENCE_TIME FpsToInterval(int fps) {
    if (fps < 10) fps = 10;
    if (fps > 60) fps = 60;
    return 10000000LL / fps;
}

static inline uint8_t ClampByte(int v) {
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Valida con rigor absoluto que las dimensiones propuestas coincidan con
// (activeW x activeH), aceptando cualquier AvgTimePerFrame (10 a 60 FPS).
static bool ParseMediaType(const CMediaType* pmt, int activeW, int activeH, int activeFps, StreamConfig* outCfg) {
    if (!pmt || !outCfg) return false;
    if (*pmt->Type() != MEDIATYPE_Video) return false;

    OutputPixFmt pixFmt = PIX_RGB32;
    const GUID sub = *pmt->Subtype();
    if (sub == MEDIASUBTYPE_RGB32)       pixFmt = PIX_RGB32;
    else if (sub == MEDIASUBTYPE_ARGB32) pixFmt = PIX_ARGB32;
    else if (sub == MEDIASUBTYPE_YUY2)   pixFmt = PIX_YUY2;
    else if (sub == MEDIASUBTYPE_RGB24)  pixFmt = PIX_RGB24;
    else return false;

    if (pmt->Format() == NULL || pmt->FormatLength() == 0)
        return false;

    const GUID fmtType = *pmt->FormatType();
    const BITMAPINFOHEADER* pbmi = NULL;
    REFERENCE_TIME avgTime = 0;

    if (fmtType == FORMAT_VideoInfo && pmt->FormatLength() >= sizeof(VIDEOINFOHEADER)) {
        const VIDEOINFOHEADER* pvi = (const VIDEOINFOHEADER*)pmt->Format();
        pbmi = &pvi->bmiHeader;
        avgTime = pvi->AvgTimePerFrame;
    }
    else if (fmtType == FORMAT_VideoInfo2 && pmt->FormatLength() >= sizeof(VIDEOINFOHEADER2)) {
        const VIDEOINFOHEADER2* pvi2 = (const VIDEOINFOHEADER2*)pmt->Format();
        pbmi = &pvi2->bmiHeader;
        avgTime = pvi2->AvgTimePerFrame;
    }
    else {
        return false;
    }

    WORD expectedBits = BitCountFromPixFmt(pixFmt);
    if (pbmi->biBitCount != 0 && pbmi->biBitCount != expectedBits)
        return false;

    long w = pbmi->biWidth;
    long rawH = pbmi->biHeight;
    long h = rawH < 0 ? -rawH : rawH;

    // Imposicion estricta de dimensiones nativas del sensor:
    // Prohibido aceptar 1280x720, 640x360 o cualquier tamano distinto a (activeW x activeH).
    if (w != activeW || h != activeH)
        return false;

    // Flexibilidad total de FPS (10 a 60 FPS) para que WebRTC/Discord no descarten 960p
    int fps = activeFps;
    if (avgTime > 0) {
        fps = (int)((10000000LL + (avgTime / 2)) / avgTime);
        if (fps < 10) fps = 10;
        if (fps > 60) fps = 60;
    }

    outCfg->width = (int)w;
    outCfg->height = (int)h;
    outCfg->fps = fps;
    outCfg->pixFmt = pixFmt;
    outCfg->flipRows = (pixFmt != PIX_YUY2) && (rawH > 0);
    return true;
}

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
    HRESULT BreakConnect() override;
    HRESULT OnThreadStartPlay() override;
    HRESULT OnThreadDestroy() override;
    HRESULT STDMETHODCALLTYPE SetFormat(AM_MEDIA_TYPE* pmt) override;
    HRESULT STDMETHODCALLTYPE GetFormat(AM_MEDIA_TYPE** ppmt) override;
    HRESULT STDMETHODCALLTYPE GetNumberOfCapabilities(int* piCount, int* piSize) override;
    HRESULT STDMETHODCALLTYPE GetStreamCaps(int iIndex, AM_MEDIA_TYPE** pmt, BYTE* pSCC) override;
    HRESULT STDMETHODCALLTYPE Set(REFGUID guidPropSet, DWORD dwID, void* pInstanceData, DWORD cbInstanceData, void* pPropData, DWORD cbPropData) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Get(REFGUID guidPropSet, DWORD dwPropID, void* pInstanceData, DWORD cbInstanceData, void* pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    HRESULT STDMETHODCALLTYPE QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;
private:
    enum CopyResult { COPY_OK, COPY_NO_SIGNAL, COPY_BUSY };

    static HRESULT BuildMediaTypeFromConfig(const StreamConfig& cfg, CMediaType* pmt);
    void TryOpenSharedMemory(bool force);
    void CloseSharedMemory();
    void QueryActiveSensorMode(int* outW, int* outH, int* outFps);
    StreamConfig GetDynamicCap(int index);
    void PublishClientRequest(int width, int height);
    void WaitForNextFrame(int fps);
    CopyResult CopyFrameFromSharedMemory(BYTE* dst, const StreamConfig& cfg);
    static void FillBlackFrame(BYTE* dst, const StreamConfig& cfg);
    static void CopyNativeFrame(const BYTE* srcPixels, BYTE* dst, const StreamConfig& cfg);

    HANDLE m_hSharedMemory;
    void* m_pSharedBuffer;
    bool m_writable;
    ULONGLONG m_lastOpenTry;
    LONGLONG m_qpcFreq;
    LONGLONG m_nextDue;
    bool m_hasCustomFormat;
    StreamConfig m_preferredCfg;
    StreamConfig m_activeCfg;
    bool m_firstSample;
    bool m_hasValidFrame;
    REFERENCE_TIME m_rtStreamTime;
};

class CKinectVirtualCam : public CSource {
public: static CUnknown* WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT* phr);
private: CKinectVirtualCam(LPUNKNOWN lpunk, HRESULT* phr);
};

CKinectVirtualCam::CKinectVirtualCam(LPUNKNOWN lpunk, HRESULT* phr)
    : CSource(NAME("Kinect Virtual Cam"), lpunk, CLSID_KinectVirtualCam) {
    new CKinectStream(phr, this, L"Video Out");
}

CUnknown* WINAPI CKinectVirtualCam::CreateInstance(LPUNKNOWN lpunk, HRESULT* phr) {
    return new CKinectVirtualCam(lpunk, phr);
}

CKinectStream::CKinectStream(HRESULT* phr, CSource* pParent, LPCWSTR pPinName)
    : CSourceStream(NAME("Video Out"), phr, pParent, pPinName),
    m_hSharedMemory(NULL), m_pSharedBuffer(NULL), m_writable(false), m_lastOpenTry(0),
    m_qpcFreq(1), m_nextDue(0), m_hasCustomFormat(false),
    m_firstSample(true), m_hasValidFrame(false), m_rtStreamTime(0) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); m_qpcFreq = f.QuadPart;
    timeBeginPeriod(1);
    TryOpenSharedMemory(true);
    m_preferredCfg = GetDynamicCap(0);
    m_activeCfg = m_preferredCfg;
    BuildMediaTypeFromConfig(m_activeCfg, &m_mt);
}

CKinectStream::~CKinectStream() {
    PublishClientRequest(0, 0);
    CloseSharedMemory();
    timeEndPeriod(1);
}

STDMETHODIMP CKinectStream::NonDelegatingQueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IAMStreamConfig) return GetInterface((IAMStreamConfig*)this, ppv);
    else if (riid == IID_IKsPropertySet) return GetInterface((IKsPropertySet*)this, ppv);
    return CSourceStream::NonDelegatingQueryInterface(riid, ppv);
}

void CKinectStream::CloseSharedMemory() {
    if (m_pSharedBuffer) { UnmapViewOfFile(m_pSharedBuffer); m_pSharedBuffer = NULL; }
    if (m_hSharedMemory) { CloseHandle(m_hSharedMemory); m_hSharedMemory = NULL; }
    m_writable = false;
}

void CKinectStream::TryOpenSharedMemory(bool force) {
    if (m_pSharedBuffer) return;
    ULONGLONG now = GetTickCount64();
    if (!force && now - m_lastOpenTry < 500) return;
    m_lastOpenTry = now;

    bool writable = true;
    HANDLE h = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, "KinectSharedMem");
    if (!h) { writable = false; h = OpenFileMappingA(FILE_MAP_READ, FALSE, "KinectSharedMem"); }
    if (!h) return;

    void* p = MapViewOfFile(h, writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(h); return; }

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < (SIZE_T)KINECT_IPC_TOTAL_SIZE) {
        UnmapViewOfFile(p); CloseHandle(h);
        return;
    }

    m_hSharedMemory = h;
    m_pSharedBuffer = p;
    m_writable = writable;
}

void CKinectStream::QueryActiveSensorMode(int* outW, int* outH, int* outFps) {
    if (!m_pSharedBuffer) TryOpenSharedMemory(true);
    if (m_pSharedBuffer) {
        const KinectIpcHeader* hdr = (const KinectIpcHeader*)m_pSharedBuffer;
        if (hdr->magic == KINECT_IPC_MAGIC) {
            const int w = *(volatile const int32_t*)&hdr->width;
            const int h = *(volatile const int32_t*)&hdr->height;
            if (w == 1280 && h == 960) {
                if (outW) *outW = 1280;
                if (outH) *outH = 960;
                if (outFps) *outFps = 12;
                return;
            }
        }
    }
    if (outW) *outW = 640;
    if (outH) *outH = 480;
    if (outFps) *outFps = 30;
}

StreamConfig CKinectStream::GetDynamicCap(int index) {
    if (index < 0 || index >= g_pixFmtCount) index = 0;
    int w = 640, h = 480, fps = 30;
    QueryActiveSensorMode(&w, &h, &fps);
    StreamConfig cfg;
    cfg.width = w;
    cfg.height = h;
    cfg.fps = fps;
    cfg.pixFmt = g_pixFmts[index];
    cfg.flipRows = false;
    return cfg;
}

// Solo se invoca en SetMediaType (conexion exitosa), FillBuffer (transmision activa)
// y al desconectar/destruir (0, 0). Nunca se invoca en CheckMediaType ni SetFormat.
void CKinectStream::PublishClientRequest(int width, int height) {
    if (!m_pSharedBuffer) TryOpenSharedMemory(false);
    if (!m_pSharedBuffer || !m_writable) return;
    KinectIpcHeader* hdr = (KinectIpcHeader*)m_pSharedBuffer;
    hdr->reqWidth = width;
    hdr->reqHeight = height;
}

HRESULT CKinectStream::BuildMediaTypeFromConfig(const StreamConfig& cfg, CMediaType* pmt) {
    if (!pmt) return E_POINTER;

    pmt->SetType(&MEDIATYPE_Video);
    pmt->SetSubtype(SubtypeFromPixFmt(cfg.pixFmt));
    pmt->SetFormatType(&FORMAT_VideoInfo);
    pmt->SetTemporalCompression(FALSE);

    VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)pmt->AllocFormatBuffer(sizeof(VIDEOINFOHEADER));
    if (!pvi) return E_OUTOFMEMORY;
    ZeroMemory(pvi, sizeof(VIDEOINFOHEADER));

    const DWORD imageSize = (DWORD)CalcImageSize(cfg);
    pvi->rcSource.right = cfg.width;  pvi->rcSource.bottom = cfg.height;
    pvi->rcTarget.right = cfg.width;  pvi->rcTarget.bottom = cfg.height;
    pvi->dwBitRate = (DWORD)((LONGLONG)imageSize * 8 * cfg.fps);
    pvi->AvgTimePerFrame = FpsToInterval(cfg.fps);
    pvi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pvi->bmiHeader.biWidth = cfg.width;
    if (cfg.pixFmt == PIX_YUY2)
        pvi->bmiHeader.biHeight = cfg.height;
    else
        pvi->bmiHeader.biHeight = cfg.flipRows ? cfg.height : -cfg.height;

    pvi->bmiHeader.biPlanes = 1;
    pvi->bmiHeader.biBitCount = BitCountFromPixFmt(cfg.pixFmt);
    pvi->bmiHeader.biCompression = CompressionFromPixFmt(cfg.pixFmt);
    pvi->bmiHeader.biSizeImage = imageSize;

    pmt->SetSampleSize(imageSize);
    return S_OK;
}

HRESULT CKinectStream::CheckMediaType(const CMediaType* pmt) {
    if (!pmt) return E_POINTER;
    int activeW = 640, activeH = 480, activeFps = 30;
    QueryActiveSensorMode(&activeW, &activeH, &activeFps);
    StreamConfig parsed;
    if (!ParseMediaType(pmt, activeW, activeH, activeFps, &parsed))
        return VFW_E_INVALIDMEDIATYPE;
    return S_OK;
}

HRESULT CKinectStream::GetMediaType(int iPosition, CMediaType* pmt) {
    if (!pmt) return E_POINTER;
    if (iPosition < 0) return E_INVALIDARG;

    int activeW = 640, activeH = 480, activeFps = 30;
    QueryActiveSensorMode(&activeW, &activeH, &activeFps);

    const bool useCustom = m_hasCustomFormat &&
        m_preferredCfg.width == activeW &&
        m_preferredCfg.height == activeH;

    if (!useCustom) {
        if (iPosition >= g_pixFmtCount) return VFW_S_NO_MORE_ITEMS;
        return BuildMediaTypeFromConfig(GetDynamicCap(iPosition), pmt);
    }

    if (iPosition == 0) return BuildMediaTypeFromConfig(m_preferredCfg, pmt);
    int idx = iPosition - 1;
    if (idx >= g_pixFmtCount) return VFW_S_NO_MORE_ITEMS;
    return BuildMediaTypeFromConfig(GetDynamicCap(idx), pmt);
}

HRESULT CKinectStream::SetMediaType(const CMediaType* pMediaType) {
    int activeW = 640, activeH = 480, activeFps = 30;
    QueryActiveSensorMode(&activeW, &activeH, &activeFps);

    StreamConfig parsed;
    if (!ParseMediaType(pMediaType, activeW, activeH, activeFps, &parsed))
        return VFW_E_INVALIDMEDIATYPE;

    HRESULT hr = CSourceStream::SetMediaType(pMediaType);
    if (FAILED(hr)) return hr;

    m_activeCfg = parsed;
    PublishClientRequest(m_activeCfg.width, m_activeCfg.height);
    return S_OK;
}

HRESULT CKinectStream::BreakConnect() {
    PublishClientRequest(0, 0);
    return CSourceStream::BreakConnect();
}

HRESULT STDMETHODCALLTYPE CKinectStream::SetFormat(AM_MEDIA_TYPE* pmt) {
    CAutoLock lock(m_pFilter->pStateLock());

    if (!pmt) {
        m_hasCustomFormat = false;
        m_preferredCfg = GetDynamicCap(0);
        if (!IsConnected()) {
            m_activeCfg = m_preferredCfg;
            BuildMediaTypeFromConfig(m_activeCfg, &m_mt);
        }
        return S_OK;
    }

    int activeW = 640, activeH = 480, activeFps = 30;
    QueryActiveSensorMode(&activeW, &activeH, &activeFps);

    CMediaType mt(*pmt);
    StreamConfig parsed;
    if (!ParseMediaType(&mt, activeW, activeH, activeFps, &parsed))
        return VFW_E_INVALIDMEDIATYPE;

    if (m_pFilter->IsActive()) {
        if (parsed.width == m_activeCfg.width && parsed.height == m_activeCfg.height && parsed.pixFmt == m_activeCfg.pixFmt) {
            m_activeCfg.fps = parsed.fps;
            return S_OK;
        }
        return VFW_E_WRONG_STATE;
    }

    if (parsed.pixFmt != PIX_YUY2) parsed.flipRows = false;

    m_hasCustomFormat = true;
    m_preferredCfg = parsed;
    m_activeCfg = parsed;
    HRESULT hr = BuildMediaTypeFromConfig(parsed, &m_mt);
    if (FAILED(hr)) return hr;

    if (IsConnected() && m_pFilter->GetFilterGraph()) {
        m_pFilter->GetFilterGraph()->Reconnect(this);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CKinectStream::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    CAutoLock lock(m_pFilter->pStateLock());

    if (!IsConnected()) {
        int activeW = 640, activeH = 480, activeFps = 30;
        QueryActiveSensorMode(&activeW, &activeH, &activeFps);
        if (!m_hasCustomFormat || m_activeCfg.width != activeW || m_activeCfg.height != activeH) {
            m_hasCustomFormat = false;
            m_preferredCfg = GetDynamicCap(0);
            m_activeCfg = m_preferredCfg;
            BuildMediaTypeFromConfig(m_activeCfg, &m_mt);
        }
    }

    *ppmt = CreateMediaType(&m_mt);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE CKinectStream::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = g_pixFmtCount;
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CKinectStream::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** pmt, BYTE* pSCC) {
    if (!pmt || !pSCC) return E_POINTER;
    if (iIndex < 0) return E_INVALIDARG;
    if (iIndex >= g_pixFmtCount) return S_FALSE;

    const StreamConfig cfg = GetDynamicCap(iIndex);
    CMediaType mt;
    HRESULT hr = BuildMediaTypeFromConfig(cfg, &mt);
    if (FAILED(hr)) return hr;
    *pmt = CreateMediaType(&mt);
    if (!*pmt) return E_OUTOFMEMORY;

    const long imgBytes = CalcImageSize(cfg);
    VIDEO_STREAM_CONFIG_CAPS* pvscc = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
    ZeroMemory(pvscc, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    pvscc->guid = FORMAT_VideoInfo;
    pvscc->VideoStandard = AnalogVideo_None;

    pvscc->InputSize.cx = cfg.width;       pvscc->InputSize.cy = cfg.height;
    pvscc->MinCroppingSize.cx = cfg.width; pvscc->MinCroppingSize.cy = cfg.height;
    pvscc->MaxCroppingSize.cx = cfg.width; pvscc->MaxCroppingSize.cy = cfg.height;
    pvscc->MinOutputSize.cx = cfg.width;   pvscc->MinOutputSize.cy = cfg.height;
    pvscc->MaxOutputSize.cx = cfg.width;   pvscc->MaxOutputSize.cy = cfg.height;
    pvscc->CropGranularityX = 1;           pvscc->CropGranularityY = 1;
    pvscc->OutputGranularityX = 1;         pvscc->OutputGranularityY = 1;
    // Rango flexible de 10 a 60 FPS para que Discord/WebRTC no descarten 960p
    pvscc->MinFrameInterval = 166666;
    pvscc->MaxFrameInterval = 1000000;
    pvscc->MinBitsPerSecond = (LONG)((LONGLONG)imgBytes * 8 * 10);
    pvscc->MaxBitsPerSecond = (LONG)((LONGLONG)imgBytes * 8 * 60);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CKinectStream::Get(REFGUID guidPropSet, DWORD dwPropID, void* pInstanceData, DWORD cbInstanceData, void* pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pcbReturned) *pcbReturned = sizeof(GUID);
    if (!pPropData) return S_OK;
    if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
    *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CKinectStream::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

HRESULT CKinectStream::DecideBufferSize(IMemAllocator* pAlloc, ALLOCATOR_PROPERTIES* pProperties) {
    CAutoLock filterLock(m_pFilter->pStateLock());
    const long needed = CalcImageSize(m_activeCfg);
    if (pProperties->cBuffers < 2) pProperties->cBuffers = 2;
    if (pProperties->cbBuffer < needed) pProperties->cbBuffer = needed;
    if (pProperties->cbAlign == 0) pProperties->cbAlign = 1;

    ALLOCATOR_PROPERTIES Actual;
    ZeroMemory(&Actual, sizeof(Actual));
    HRESULT hr = pAlloc->SetProperties(pProperties, &Actual);
    if (FAILED(hr)) {
        if (FAILED(pAlloc->GetProperties(&Actual))) return hr;
    }
    return (Actual.cbBuffer >= needed) ? S_OK : E_FAIL;
}

HRESULT CKinectStream::OnThreadStartPlay() {
    m_rtStreamTime = 0;
    m_nextDue = 0;
    m_firstSample = true;
    m_hasValidFrame = false;
    PublishClientRequest(m_activeCfg.width, m_activeCfg.height);
    return CSourceStream::OnThreadStartPlay();
}

HRESULT CKinectStream::OnThreadDestroy() {
    PublishClientRequest(0, 0);
    return CSourceStream::OnThreadDestroy();
}

void CKinectStream::WaitForNextFrame(int fps) {
    if (fps < 5) fps = 30;
    const LONGLONG interval = m_qpcFreq / fps;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (m_nextDue == 0) m_nextDue = now.QuadPart;

    LONGLONG remaining = m_nextDue - now.QuadPart;
    while (remaining > 0) {
        LONGLONG ms = remaining * 1000 / m_qpcFreq;
        Sleep(ms >= 2 ? (DWORD)(ms - 1) : 0);
        QueryPerformanceCounter(&now);
        remaining = m_nextDue - now.QuadPart;
    }

    m_nextDue += interval;
    if (now.QuadPart - m_nextDue > interval * 2) m_nextDue = now.QuadPart + interval;
}

void CKinectStream::FillBlackFrame(BYTE* dst, const StreamConfig& cfg) {
    if (cfg.pixFmt == PIX_YUY2) {
        uint32_t* d = (uint32_t*)dst;
        long macroPixels = (long)cfg.width * cfg.height / 2;
        for (long i = 0; i < macroPixels; ++i) d[i] = 0x80108010u;
    }
    else if (cfg.pixFmt == PIX_RGB24) {
        memset(dst, 0, (size_t)CalcImageSize(cfg));
    }
    else {
        uint32_t* d = (uint32_t*)dst;
        long pixels = (long)cfg.width * cfg.height;
        for (long i = 0; i < pixels; ++i) d[i] = 0xFF000000u;
    }
}

// Copia 1:1 a cuadro completo (100% del sensor a 100% del buffer del cliente,
// sin escalado, sin centrado en sub-caja y sin recorte de campo de vision).
void CKinectStream::CopyNativeFrame(const BYTE* srcPixels, BYTE* dst, const StreamConfig& cfg) {
    const int width = cfg.width;
    const int height = cfg.height;
    const size_t srcStride = (size_t)width * 4;
    const size_t dstStride = (size_t)CalcStride(width, cfg.pixFmt);

    if ((cfg.pixFmt == PIX_RGB32 || cfg.pixFmt == PIX_ARGB32) && !cfg.flipRows) {
        memcpy(dst, srcPixels, dstStride * (size_t)height);
        return;
    }

    for (int y = 0; y < height; ++y) {
        const BYTE* sRow = srcPixels + (size_t)y * srcStride;
        const int outY = cfg.flipRows ? (height - 1 - y) : y;
        BYTE* dRow = dst + (size_t)outY * dstStride;

        if (cfg.pixFmt == PIX_RGB32 || cfg.pixFmt == PIX_ARGB32) {
            memcpy(dRow, sRow, srcStride);
        }
        else if (cfg.pixFmt == PIX_RGB24) {
            BYTE* d = dRow;
            const BYTE* s = sRow;
            for (int x = 0; x < width; ++x, s += 4, d += 3) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        }
        else { // PIX_YUY2
            BYTE* d = dRow;
            const BYTE* s = sRow;
            for (int x = 0; x < width; x += 2, s += 8, d += 4) {
                const int b0 = s[0], g0 = s[1], r0 = s[2];
                const int b1 = s[4], g1 = s[5], r1 = s[6];
                const int y0 = (( 66 * r0 + 129 * g0 +  25 * b0 + 128) >> 8) + 16;
                const int y1 = (( 66 * r1 + 129 * g1 +  25 * b1 + 128) >> 8) + 16;
                const int rA = (r0 + r1) >> 1;
                const int gA = (g0 + g1) >> 1;
                const int bA = (b0 + b1) >> 1;
                const int u  = ((-38 * rA -  74 * gA + 112 * bA + 128) >> 8) + 128;
                const int v  = ((112 * rA -  94 * gA -  18 * bA + 128) >> 8) + 128;
                d[0] = ClampByte(y0);
                d[1] = ClampByte(u);
                d[2] = ClampByte(y1);
                d[3] = ClampByte(v);
            }
        }
    }
}

static inline int64_t ReadSequenceAtomic(volatile const int64_t* p) {
#if defined(_M_X64) || defined(_M_ARM64)
    return *p;
#else
    const volatile uint32_t* w = (const volatile uint32_t*)p;
    for (;;) {
        uint32_t hi1 = w[1];
        MemoryBarrier();
        uint32_t lo = w[0];
        MemoryBarrier();
        uint32_t hi2 = w[1];
        if (hi1 == hi2) return ((int64_t)hi1 << 32) | lo;
    }
#endif
}

CKinectStream::CopyResult CKinectStream::CopyFrameFromSharedMemory(BYTE* dst, const StreamConfig& cfg) {
    if (!m_pSharedBuffer) return COPY_NO_SIGNAL;

    const KinectIpcHeader* hdr = (const KinectIpcHeader*)m_pSharedBuffer;
    const BYTE* pixels = (const BYTE*)m_pSharedBuffer + KINECT_IPC_HEADER_SIZE;

    for (int attempt = 0; attempt < 8; ++attempt) {
        int64_t s1 = ReadSequenceAtomic(&hdr->sequence);
        if (s1 & 1) { Sleep(1); continue; }
        MemoryBarrier();

        const int srcW = hdr->width;
        const int srcH = hdr->height;
        const int srcStride = hdr->stride;
        if (hdr->magic != KINECT_IPC_MAGIC || hdr->active == 0 ||
            srcW != cfg.width || srcH != cfg.height ||
            srcStride != srcW * 4) {
            return COPY_NO_SIGNAL;
        }

        CopyNativeFrame(pixels, dst, cfg);

        MemoryBarrier();
        int64_t s2 = ReadSequenceAtomic(&hdr->sequence);
        if (s1 == s2) return COPY_OK;
    }
    return COPY_BUSY;
}

HRESULT CKinectStream::FillBuffer(IMediaSample* pms) {
    if (!pms) return E_POINTER;

    const StreamConfig cfg = m_activeCfg;
    WaitForNextFrame(cfg.fps);

    BYTE* pData = NULL;
    HRESULT hr = pms->GetPointer(&pData);
    if (FAILED(hr) || !pData) return E_FAIL;

    const long frameBytes = CalcImageSize(cfg);
    if (pms->GetSize() < frameBytes) return E_FAIL;

    TryOpenSharedMemory(false);
    PublishClientRequest(cfg.width, cfg.height);

    CopyResult res = CopyFrameFromSharedMemory(pData, cfg);
    if (res == COPY_OK) {
        m_hasValidFrame = true;
    }
    else if (res == COPY_NO_SIGNAL || !m_hasValidFrame) {
        FillBlackFrame(pData, cfg);
        m_hasValidFrame = false;
    }

    pms->SetActualDataLength(frameBytes);

    const REFERENCE_TIME interval = FpsToInterval(cfg.fps);
    REFERENCE_TIME rtStart = m_rtStreamTime;
    m_rtStreamTime += interval;
    REFERENCE_TIME rtStop = m_rtStreamTime;
    pms->SetTime(&rtStart, &rtStop);
    pms->SetSyncPoint(TRUE);
    pms->SetDiscontinuity(m_firstSample ? TRUE : FALSE);
    m_firstSample = false;
    return S_OK;
}

const REGPINTYPES sudPinTypes[] = {
    { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB32 },
    { &MEDIATYPE_Video, &MEDIASUBTYPE_YUY2 },
    { &MEDIATYPE_Video, &MEDIASUBTYPE_ARGB32 },
    { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB24 }
};
const REGFILTERPINS sudpPins = { (LPWSTR)L"Output", FALSE, TRUE, FALSE, FALSE, &CLSID_NULL, NULL, 4, sudPinTypes };
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
        REGFILTER2 rf2;
        ZeroMemory(&rf2, sizeof(rf2));
        rf2.dwVersion = 1;
        rf2.dwMerit = MERIT_DO_NOT_USE;
        rf2.cPins = 1;
        rf2.rgPins = &sudpPins;
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
extern "C" BOOL APIENTRY DllMain(HANDLE hModule, DWORD dwReason, LPVOID lpReserved) { return DllEntryPoint((HINSTANCE)(hModule), dwReason, lpReserved); }
