// PlayLeagueCam.dll: eight virtual cameras ("Play RX Pilot 1" ... "Pilot 8") for Windows programs that take a
// webcam / capture device through DirectShow: FPV Trackside (its DirectShow video framework), OBS (Video Capture
// Device), ffmpeg -f dshow, ... Each camera is a DirectShow live source filter with one capture pin, 640 x 480
// (4:3, square pixels), RGB32 or YUY2, 60 or 30 fps. Pictures come from the app through shared memory
// (vcam_shared.h): no compression, no network, so next to no delay. While the app is not running, or the slot has
// no pilot / no picture, the camera shows a "no video" card (so the program reading it never stalls).
//
// Registered per user (no admin): DllInstall(TRUE, L"Play RX") writes HKCU\Software\Classes (the app does it at
// every start, so the path is always right); DllInstall(FALSE, ...) removes it.
#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksproxy.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "vcam_shared.h"

namespace {
using namespace arx;

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0}, g_locks{0};

// the cameras' class ids: one per pilot slot (the last byte is the slot)
GUID camera_clsid(int slot) {
  GUID g = {0xa1c2e3f4, 0x5b6d, 0x4e7f, {0x8a, 0x9b, 0x0c, 0x1d, 0x2e, 0x3f, 0x4a, 0x00}};
  g.Data4[7] = static_cast<unsigned char>(slot);
  return g;
}
int slot_of(REFCLSID c) {
  for (int n = 1; n <= vcam::kSlots; n++)
    if (IsEqualGUID(c, camera_clsid(n))) return n;
  return 0;
}

// GUIDs used here, spelled out (MinGW's import libraries do not carry all of them)
const GUID kVideoInputCategory = {0x860BB310, 0x5D01, 0x11D0, {0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86}};
const GUID kMediaTypeVideo = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubRGB32 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
const GUID kSubYUY2 = {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kFormatVideoInfo = {0x05589f80, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
const GUID kPinCategoryCapture = {0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};
const GUID kPropSetPin = {0x9b00f101, 0x1567, 0x11d1, {0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5}};
const GUID kMemoryAllocator = {0x1e651cc0, 0xb199, 0x11d0, {0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45}};
const IID kIID_IAMStreamConfig = {0xc6e13340, 0x30ac, 0x11d0, {0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56}};
const IID kIID_IKsPropertySet = {0x31efac30, 0x515c, 0x11d0, {0xa9, 0xaa, 0x00, 0xaa, 0x00, 0x61, 0xbe, 0x93}};
const IID kIID_IAMFilterMiscFlags = {0x2dd74950, 0xa890, 0x11d1, {0xab, 0xe8, 0x00, 0xa0, 0xc9, 0x05, 0xf3, 0x75}};
constexpr DWORD kPinCategoryProp = 0;  // AMPROPERTY_PIN_CATEGORY
constexpr HRESULT kPropSetUnsupported = static_cast<HRESULT>(0x80070492L);
constexpr HRESULT kPropIdUnsupported = static_cast<HRESULT>(0x80070490L);

// ---------------------------------------------------------------- media types
constexpr int kTypes = 4;  // RGB32 60, RGB32 30, YUY2 60, YUY2 30
constexpr REFERENCE_TIME kT60 = 166667, kT30 = 333333;

void free_mt(AM_MEDIA_TYPE &mt) {
  if (mt.pbFormat) CoTaskMemFree(mt.pbFormat);
  mt.pbFormat = nullptr;
  mt.cbFormat = 0;
  if (mt.pUnk) mt.pUnk->Release();
  mt.pUnk = nullptr;
}
bool copy_mt(AM_MEDIA_TYPE &to, const AM_MEDIA_TYPE &from) {
  to = from;
  to.pUnk = nullptr;
  to.pbFormat = nullptr;
  if (from.cbFormat && from.pbFormat) {
    to.pbFormat = static_cast<BYTE *>(CoTaskMemAlloc(from.cbFormat));
    if (!to.pbFormat) return false;
    std::memcpy(to.pbFormat, from.pbFormat, from.cbFormat);
  }
  return true;
}
AM_MEDIA_TYPE *new_mt(const AM_MEDIA_TYPE &from) {
  auto *p = static_cast<AM_MEDIA_TYPE *>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
  if (p && !copy_mt(*p, from)) {
    CoTaskMemFree(p);
    return nullptr;
  }
  return p;
}
void delete_mt(AM_MEDIA_TYPE *p) {
  if (!p) return;
  free_mt(*p);
  CoTaskMemFree(p);
}
bool make_type(int i, AM_MEDIA_TYPE &mt) {
  if (i < 0 || i >= kTypes) return false;
  const bool yuy2 = i >= 2;
  const REFERENCE_TIME t = (i % 2 == 0) ? kT60 : kT30;
  std::memset(&mt, 0, sizeof mt);
  auto *vih = static_cast<VIDEOINFOHEADER *>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
  if (!vih) return false;
  std::memset(vih, 0, sizeof *vih);
  vih->AvgTimePerFrame = t;
  BITMAPINFOHEADER &b = vih->bmiHeader;
  b.biSize = sizeof(BITMAPINFOHEADER);
  b.biWidth = vcam::kOutW;
  b.biHeight = vcam::kOutH;  // RGB: bottom-up rows; YUY2: top-down whatever the sign
  b.biPlanes = 1;
  b.biBitCount = yuy2 ? 16 : 32;
  b.biCompression = yuy2 ? MAKEFOURCC('Y', 'U', 'Y', '2') : BI_RGB;
  b.biSizeImage = static_cast<DWORD>(vcam::kOutW) * vcam::kOutH * b.biBitCount / 8;
  vih->dwBitRate = static_cast<DWORD>(static_cast<double>(b.biSizeImage) * 8 * 10000000.0 / static_cast<double>(t));
  mt.majortype = kMediaTypeVideo;
  mt.subtype = yuy2 ? kSubYUY2 : kSubRGB32;
  mt.bFixedSizeSamples = TRUE;
  mt.bTemporalCompression = FALSE;
  mt.lSampleSize = b.biSizeImage;
  mt.formattype = kFormatVideoInfo;
  mt.cbFormat = sizeof(VIDEOINFOHEADER);
  mt.pbFormat = reinterpret_cast<BYTE *>(vih);
  return true;
}
// one of ours? (any frame interval: it is only how fast we send)
bool acceptable(const AM_MEDIA_TYPE *mt) {
  if (!mt || !IsEqualGUID(mt->majortype, kMediaTypeVideo)) return false;
  if (!IsEqualGUID(mt->subtype, kSubRGB32) && !IsEqualGUID(mt->subtype, kSubYUY2)) return false;
  if (!IsEqualGUID(mt->formattype, kFormatVideoInfo) || mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat) return false;
  const auto *v = reinterpret_cast<const VIDEOINFOHEADER *>(mt->pbFormat);
  return v->bmiHeader.biWidth == vcam::kOutW && std::abs(v->bmiHeader.biHeight) == vcam::kOutH;
}
REFERENCE_TIME frame_time(const AM_MEDIA_TYPE &mt) {
  const auto *v = reinterpret_cast<const VIDEOINFOHEADER *>(mt.pbFormat);
  const REFERENCE_TIME t = v ? v->AvgTimePerFrame : 0;
  return t >= 100000 && t <= 2000000 ? t : kT60;
}

// ---------------------------------------------------------------- pictures
// The slot's newest picture as a 640 x 480 BGRX top-down frame. False: no picture (the card is drawn instead).
class Source {
public:
  explicit Source(int slot) : _slot(slot) {}
  ~Source() {
    if (_view) UnmapViewOfFile(_view);
    if (_map) CloseHandle(_map);
  }
  bool picture(std::vector<uint8_t> &out, std::string &pilot) {
    if (!_view) {
      if (GetTickCount64() < _retry) return false;
      _retry = GetTickCount64() + 1000;
      wchar_t nm[64];
      vcam::name_of(_slot, nm, 64);
      _map = OpenFileMappingW(FILE_MAP_READ, FALSE, nm);
      if (!_map) return false;
      _view = MapViewOfFile(_map, FILE_MAP_READ, 0, 0, vcam::kBytes);
      if (!_view) {
        CloseHandle(_map);
        _map = nullptr;
        return false;
      }
    }
    const auto *h = static_cast<const vcam::Header *>(_view);
    if (h->magic != vcam::kMagic) return false;
    pilot.assign(h->name, strnlen(h->name, sizeof h->name));
    if (!h->active || static_cast<int64_t>(GetTickCount64()) - h->written_ms > 1000) return false;
    for (int tries = 0; tries < 4; tries++) {
      const long s1 = h->seq;
      if (s1 & 1) {
        Sleep(0);
        continue;
      }
      const int w = static_cast<int>(h->width), hh = static_cast<int>(h->height);
      if (w < 2 || hh < 2 || w > vcam::kMaxW || hh > vcam::kMaxH) return false;
      _raw.resize(static_cast<size_t>(w) * hh * 4);
      std::memcpy(_raw.data(), reinterpret_cast<const uint8_t *>(h + 1), _raw.size());
      MemoryBarrier();
      if (h->seq != s1) continue;  // the writer moved on mid-copy: again
      scale(_raw.data(), w, hh, out);
      return true;
    }
    return false;
  }

private:
  static void scale(const uint8_t *src, int sw, int sh, std::vector<uint8_t> &dst) {  // bilinear, into 640 x 480
    constexpr int W = vcam::kOutW, H = vcam::kOutH;
    dst.resize(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; y++) {
      const float fy = (y + 0.5f) * sh / H - 0.5f;
      const int y0 = std::clamp(static_cast<int>(fy), 0, sh - 2);
      const float wy = std::clamp(fy - y0, 0.f, 1.f);
      const uint8_t *r0 = src + static_cast<size_t>(y0) * sw * 4, *r1 = r0 + static_cast<size_t>(sw) * 4;
      uint8_t *d = dst.data() + static_cast<size_t>(y) * W * 4;
      for (int x = 0; x < W; x++, d += 4) {
        const float fx = (x + 0.5f) * sw / W - 0.5f;
        const int x0 = std::clamp(static_cast<int>(fx), 0, sw - 2);
        const float wx = std::clamp(fx - x0, 0.f, 1.f);
        const uint8_t *a = r0 + x0 * 4, *b = r1 + x0 * 4;
        for (int c = 0; c < 3; c++) {
          const float top = a[c] + (a[c + 4] - a[c]) * wx, bot = b[c] + (b[c + 4] - b[c]) * wx;
          d[c] = static_cast<uint8_t>(top + (bot - top) * wy + 0.5f);
        }
        d[3] = 255;
      }
    }
  }
  int _slot;
  HANDLE _map = nullptr;
  void *_view = nullptr;
  ULONGLONG _retry = 0;
  std::vector<uint8_t> _raw;
};

// "Play RX Pilot 3 · no video" (and the pilot's name), drawn once with GDI
void draw_card(int slot, const std::wstring &product, const std::string &pilot, std::vector<uint8_t> &out) {
  constexpr int W = vcam::kOutW, H = vcam::kOutH;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = W;
  bi.bmiHeader.biHeight = -H;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void *bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  out.assign(static_cast<size_t>(W) * H * 4, 0);
  if (!dc || !bmp || !bits) {
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    return;
  }
  HGDIOBJ old = SelectObject(dc, bmp);
  RECT r{0, 0, W, H};
  HBRUSH bg = CreateSolidBrush(RGB(24, 24, 27));
  FillRect(dc, &r, bg);
  DeleteObject(bg);
  SetBkMode(dc, TRANSPARENT);
  HFONT big = CreateFontW(44, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  HFONT small = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  std::wstring who;
  if (!pilot.empty()) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, pilot.c_str(), -1, nullptr, 0);
    who.resize(static_cast<size_t>(std::max(n, 1)));
    MultiByteToWideChar(CP_UTF8, 0, pilot.c_str(), -1, who.data(), n);
    who.resize(wcslen(who.c_str()));
  }
  const std::wstring title = (product.empty() ? L"Play League" : product) + L" Pilot " + std::to_wstring(slot);
  SelectObject(dc, big);
  SetTextColor(dc, RGB(161, 161, 170));
  RECT t1{0, H / 2 - 70, W, H / 2 - 20};
  DrawTextW(dc, title.c_str(), -1, &t1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  SelectObject(dc, small);
  SetTextColor(dc, RGB(113, 113, 122));
  const std::wstring line = who.empty() ? L"no video" : who + L"  ·  no video";
  RECT t2{0, H / 2, W, H / 2 + 40};
  DrawTextW(dc, line.c_str(), -1, &t2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  GdiFlush();
  std::memcpy(out.data(), bits, out.size());
  SelectObject(dc, old);
  DeleteObject(big);
  DeleteObject(small);
  DeleteObject(bmp);
  DeleteDC(dc);
}

std::wstring product_name() {  // what the cameras were registered as ("Play RX"), for the card
  wchar_t buf[128] = {};
  DWORD n = sizeof buf;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\PlayLeagueCam", L"Product", RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS)
    return buf;
  return L"Play League";
}

// ---------------------------------------------------------------- the filter and its pin
class Filter;

class Pin : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
  explicit Pin(Filter *f) : _f(f) { std::memset(&_mt, 0, sizeof _mt); std::memset(&_pref, 0, sizeof _pref); }
  ~Pin() {
    disconnect_all();
    if (_have_pref) free_mt(_pref);
  }
  // IUnknown: the pin lives and dies with its filter
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;
  // IPin
  STDMETHODIMP Connect(IPin *recv, const AM_MEDIA_TYPE *pmt) override;
  STDMETHODIMP ReceiveConnection(IPin *, const AM_MEDIA_TYPE *) override { return E_UNEXPECTED; }
  STDMETHODIMP Disconnect() override;
  STDMETHODIMP ConnectedTo(IPin **pin) override;
  STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *pmt) override;
  STDMETHODIMP QueryPinInfo(PIN_INFO *info) override;
  STDMETHODIMP QueryDirection(PIN_DIRECTION *dir) override {
    if (!dir) return E_POINTER;
    *dir = PINDIR_OUTPUT;
    return S_OK;
  }
  STDMETHODIMP QueryId(LPWSTR *id) override {
    if (!id) return E_POINTER;
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(L"Capture")));
    if (!*id) return E_OUTOFMEMORY;
    std::memcpy(*id, L"Capture", sizeof(L"Capture"));
    return S_OK;
  }
  STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *pmt) override { return acceptable(pmt) ? S_OK : S_FALSE; }
  STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **pp) override;
  STDMETHODIMP QueryInternalConnections(IPin **, ULONG *) override { return E_NOTIMPL; }
  STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
  STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
  STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }
  STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }
  // IAMStreamConfig
  STDMETHODIMP SetFormat(AM_MEDIA_TYPE *pmt) override;
  STDMETHODIMP GetFormat(AM_MEDIA_TYPE **pmt) override;
  STDMETHODIMP GetNumberOfCapabilities(int *count, int *size) override {
    if (!count || !size) return E_POINTER;
    *count = kTypes;
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
  }
  STDMETHODIMP GetStreamCaps(int i, AM_MEDIA_TYPE **pmt, BYTE *caps) override;
  // IKsPropertySet: the pin is a capture pin
  STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
  STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD *ret) override {
    if (!IsEqualGUID(set, kPropSetPin)) return kPropSetUnsupported;
    if (id != kPinCategoryProp) return kPropIdUnsupported;
    if (ret) *ret = sizeof(GUID);
    if (!data) return ret ? S_OK : E_POINTER;
    if (cb < sizeof(GUID)) return E_UNEXPECTED;
    *static_cast<GUID *>(data) = kPinCategoryCapture;
    return S_OK;
  }
  STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD *support) override {
    if (!IsEqualGUID(set, kPropSetPin)) return kPropSetUnsupported;
    if (id != kPinCategoryProp) return kPropIdUnsupported;
    if (support) *support = KSPROPERTY_SUPPORT_GET;
    return S_OK;
  }

  // the filter's state changes
  HRESULT active();    // stopped -> paused: commit the allocator
  void inactive();     // -> stopped
  void run(REFERENCE_TIME start, IReferenceClock *clock);
  void stop_streaming();

private:
  bool decide_allocator();
  void disconnect_all();
  void stream();
  Filter *_f;
  std::recursive_mutex _m;
  IPin *_peer = nullptr;
  IMemInputPin *_mip = nullptr;
  IMemAllocator *_alloc = nullptr;
  AM_MEDIA_TYPE _mt, _pref;
  bool _have_mt = false, _have_pref = false;
  std::thread _t;
  std::atomic<bool> _streaming{false};
  REFERENCE_TIME _start = 0;
  IReferenceClock *_clock = nullptr;
};

class Filter : public IBaseFilter, public IAMFilterMiscFlags {
public:
  explicit Filter(int slot) : _slot(slot), _pin(this) {
    g_objects++;
    _name = L"Pilot " + std::to_wstring(slot);
  }
  ~Filter() {
    _pin.stop_streaming();
    if (_clock) _clock->Release();
    g_objects--;
  }
  int slot() const { return _slot; }
  FILTER_STATE state() const { return _state; }
  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPersist) || IsEqualIID(riid, IID_IMediaFilter) || IsEqualIID(riid, IID_IBaseFilter))
      *ppv = static_cast<IBaseFilter *>(this);
    else if (IsEqualIID(riid, kIID_IAMFilterMiscFlags))
      *ppv = static_cast<IAMFilterMiscFlags *>(this);
    else if (IsEqualIID(riid, kIID_IAMStreamConfig) || IsEqualIID(riid, kIID_IKsPropertySet))
      return _pin.QueryInterface(riid, ppv);  // some programs ask the filter for the pin's interfaces
    else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
  STDMETHODIMP_(ULONG) Release() override {
    const long r = InterlockedDecrement(&_ref);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }
  // IPersist
  STDMETHODIMP GetClassID(CLSID *c) override {
    if (!c) return E_POINTER;
    *c = camera_clsid(_slot);
    return S_OK;
  }
  // IMediaFilter
  STDMETHODIMP Stop() override {
    std::lock_guard<std::recursive_mutex> l(_m);
    _pin.stop_streaming();
    if (_state != State_Stopped) _pin.inactive();
    _state = State_Stopped;
    return S_OK;
  }
  STDMETHODIMP Pause() override {
    std::lock_guard<std::recursive_mutex> l(_m);
    if (_state == State_Stopped) {
      const HRESULT hr = _pin.active();
      if (FAILED(hr)) return hr;
    }
    _pin.stop_streaming();  // a live source delivers only while running
    _state = State_Paused;
    return S_OK;
  }
  STDMETHODIMP Run(REFERENCE_TIME start) override {
    std::lock_guard<std::recursive_mutex> l(_m);
    if (_state == State_Stopped) {
      const HRESULT hr = Pause();
      if (FAILED(hr)) return hr;
    }
    _state = State_Running;
    _pin.run(start, _clock);
    return S_OK;
  }
  STDMETHODIMP GetState(DWORD, FILTER_STATE *s) override {
    if (!s) return E_POINTER;
    *s = _state;
    return _state == State_Paused ? VFW_S_CANT_CUE : S_OK;  // live: no data while paused
  }
  STDMETHODIMP SetSyncSource(IReferenceClock *c) override {
    std::lock_guard<std::recursive_mutex> l(_m);
    if (c) c->AddRef();
    if (_clock) _clock->Release();
    _clock = c;
    return S_OK;
  }
  STDMETHODIMP GetSyncSource(IReferenceClock **c) override {
    if (!c) return E_POINTER;
    *c = _clock;
    if (_clock) _clock->AddRef();
    return S_OK;
  }
  // IBaseFilter
  STDMETHODIMP EnumPins(IEnumPins **pp) override;
  STDMETHODIMP FindPin(LPCWSTR id, IPin **pp) override {
    if (!pp) return E_POINTER;
    if (!id || wcscmp(id, L"Capture") != 0) {
      *pp = nullptr;
      return VFW_E_NOT_FOUND;
    }
    *pp = &_pin;
    _pin.AddRef();
    return S_OK;
  }
  STDMETHODIMP QueryFilterInfo(FILTER_INFO *info) override {
    if (!info) return E_POINTER;
    wcsncpy(info->achName, _name.c_str(), MAX_FILTER_NAME - 1);
    info->achName[MAX_FILTER_NAME - 1] = 0;
    info->pGraph = _graph;
    if (_graph) _graph->AddRef();
    return S_OK;
  }
  STDMETHODIMP JoinFilterGraph(IFilterGraph *g, LPCWSTR name) override {
    _graph = g;  // not AddRef'ed: the graph owns us
    if (name) _name = name;
    return S_OK;
  }
  STDMETHODIMP QueryVendorInfo(LPWSTR *) override { return E_NOTIMPL; }
  // IAMFilterMiscFlags: a live source
  STDMETHODIMP_(ULONG) GetMiscFlags() override { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }

  Pin &pin() { return _pin; }

private:
  long _ref = 1;
  int _slot;
  Pin _pin;
  std::recursive_mutex _m;
  FILTER_STATE _state = State_Stopped;
  IReferenceClock *_clock = nullptr;
  IFilterGraph *_graph = nullptr;
  std::wstring _name;
};

// ---- enumerators
class EnumPinsImpl : public IEnumPins {
public:
  explicit EnumPinsImpl(Filter *f, ULONG pos = 0) : _f(f), _pos(pos) { _f->AddRef(); g_objects++; }
  ~EnumPinsImpl() { _f->Release(); g_objects--; }
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumPins)) {
      *ppv = static_cast<IEnumPins *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
  STDMETHODIMP_(ULONG) Release() override {
    const long r = InterlockedDecrement(&_ref);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }
  STDMETHODIMP Next(ULONG n, IPin **pins, ULONG *got) override {
    if (!pins) return E_POINTER;
    ULONG k = 0;
    while (k < n && _pos < 1) {
      pins[k] = &_f->pin();
      pins[k]->AddRef();
      k++, _pos++;
    }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG n) override {
    _pos += n;
    return _pos <= 1 ? S_OK : S_FALSE;
  }
  STDMETHODIMP Reset() override {
    _pos = 0;
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumPins **pp) override {
    if (!pp) return E_POINTER;
    *pp = new EnumPinsImpl(_f, _pos);
    return S_OK;
  }

private:
  long _ref = 1;
  Filter *_f;
  ULONG _pos;
};

class EnumTypesImpl : public IEnumMediaTypes {
public:
  explicit EnumTypesImpl(ULONG pos = 0) : _pos(pos) { g_objects++; }
  ~EnumTypesImpl() { g_objects--; }
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumMediaTypes)) {
      *ppv = static_cast<IEnumMediaTypes *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
  STDMETHODIMP_(ULONG) Release() override {
    const long r = InterlockedDecrement(&_ref);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }
  STDMETHODIMP Next(ULONG n, AM_MEDIA_TYPE **types, ULONG *got) override {
    if (!types) return E_POINTER;
    ULONG k = 0;
    while (k < n && _pos < kTypes) {
      AM_MEDIA_TYPE mt;
      if (!make_type(static_cast<int>(_pos), mt)) break;
      types[k] = new_mt(mt);
      free_mt(mt);
      if (!types[k]) break;
      k++, _pos++;
    }
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
  }
  STDMETHODIMP Skip(ULONG n) override {
    _pos += n;
    return _pos <= kTypes ? S_OK : S_FALSE;
  }
  STDMETHODIMP Reset() override {
    _pos = 0;
    return S_OK;
  }
  STDMETHODIMP Clone(IEnumMediaTypes **pp) override {
    if (!pp) return E_POINTER;
    *pp = new EnumTypesImpl(_pos);
    return S_OK;
  }

private:
  long _ref = 1;
  ULONG _pos;
};

STDMETHODIMP Filter::EnumPins(IEnumPins **pp) {
  if (!pp) return E_POINTER;
  *pp = new EnumPinsImpl(this);
  return S_OK;
}

// ---- the pin
STDMETHODIMP Pin::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IPin)) *ppv = static_cast<IPin *>(this);
  else if (IsEqualIID(riid, kIID_IAMStreamConfig)) *ppv = static_cast<IAMStreamConfig *>(this);
  else if (IsEqualIID(riid, kIID_IKsPropertySet)) *ppv = static_cast<IKsPropertySet *>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}
STDMETHODIMP_(ULONG) Pin::AddRef() { return _f->AddRef(); }
STDMETHODIMP_(ULONG) Pin::Release() { return _f->Release(); }

STDMETHODIMP Pin::Connect(IPin *recv, const AM_MEDIA_TYPE *pmt) {
  if (!recv) return E_POINTER;
  std::lock_guard<std::recursive_mutex> l(_m);
  if (_peer) return VFW_E_ALREADY_CONNECTED;
  if (_f->state() != State_Stopped) return VFW_E_NOT_STOPPED;
  // the types to offer, best first: one the caller set with SetFormat, then ours (filtered by a partial pmt)
  std::vector<AM_MEDIA_TYPE> tries;
  auto add = [&](const AM_MEDIA_TYPE &mt) {
    if (pmt && !IsEqualGUID(pmt->majortype, GUID_NULL) && !IsEqualGUID(pmt->majortype, mt.majortype)) return;
    if (pmt && !IsEqualGUID(pmt->subtype, GUID_NULL) && !IsEqualGUID(pmt->subtype, mt.subtype)) return;
    AM_MEDIA_TYPE c;
    if (copy_mt(c, mt)) tries.push_back(c);
  };
  if (pmt && acceptable(pmt)) add(*pmt);
  if (_have_pref) add(_pref);
  for (int i = 0; i < kTypes; i++) {
    AM_MEDIA_TYPE mt;
    if (make_type(i, mt)) add(mt), free_mt(mt);
  }
  HRESULT result = VFW_E_NO_ACCEPTABLE_TYPES;
  for (auto &mt : tries) {
    if (_peer) break;
    if (recv->ReceiveConnection(this, &mt) != S_OK) continue;
    _peer = recv;
    _peer->AddRef();
    copy_mt(_mt, mt);
    _have_mt = true;
    if (decide_allocator()) {
      result = S_OK;
      break;
    }
    recv->Disconnect();
    disconnect_all();
  }
  for (auto &mt : tries) free_mt(mt);
  return result;
}

bool Pin::decide_allocator() {
  if (FAILED(_peer->QueryInterface(IID_IMemInputPin, reinterpret_cast<void **>(&_mip)))) return false;
  ALLOCATOR_PROPERTIES req{}, props{}, act{};
  _mip->GetAllocatorRequirements(&req);
  props.cBuffers = std::max<long>(3, req.cBuffers);
  props.cbBuffer = std::max<long>(_mt.lSampleSize, req.cbBuffer);
  props.cbAlign = std::max<long>(1, req.cbAlign);
  props.cbPrefix = req.cbPrefix;
  IMemAllocator *a = nullptr;
  if (SUCCEEDED(_mip->GetAllocator(&a)) && a) {
    if (SUCCEEDED(a->SetProperties(&props, &act)) && act.cbBuffer >= _mt.lSampleSize && SUCCEEDED(_mip->NotifyAllocator(a, FALSE))) {
      _alloc = a;
      return true;
    }
    a->Release();
    a = nullptr;
  }
  if (FAILED(CoCreateInstance(kMemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator, reinterpret_cast<void **>(&a))))
    return false;
  if (SUCCEEDED(a->SetProperties(&props, &act)) && SUCCEEDED(_mip->NotifyAllocator(a, FALSE))) {
    _alloc = a;
    return true;
  }
  a->Release();
  return false;
}

void Pin::disconnect_all() {
  if (_alloc) _alloc->Release();
  if (_mip) _mip->Release();
  if (_peer) _peer->Release();
  _alloc = nullptr;
  _mip = nullptr;
  _peer = nullptr;
  if (_have_mt) free_mt(_mt);
  _have_mt = false;
}

STDMETHODIMP Pin::Disconnect() {
  std::lock_guard<std::recursive_mutex> l(_m);
  if (_f->state() != State_Stopped) return VFW_E_NOT_STOPPED;
  if (!_peer) return S_FALSE;
  disconnect_all();
  return S_OK;
}

STDMETHODIMP Pin::ConnectedTo(IPin **pin) {
  if (!pin) return E_POINTER;
  std::lock_guard<std::recursive_mutex> l(_m);
  *pin = _peer;
  if (!_peer) return VFW_E_NOT_CONNECTED;
  _peer->AddRef();
  return S_OK;
}

STDMETHODIMP Pin::ConnectionMediaType(AM_MEDIA_TYPE *pmt) {
  if (!pmt) return E_POINTER;
  std::lock_guard<std::recursive_mutex> l(_m);
  if (!_peer || !_have_mt) {
    std::memset(pmt, 0, sizeof *pmt);
    return VFW_E_NOT_CONNECTED;
  }
  return copy_mt(*pmt, _mt) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Pin::QueryPinInfo(PIN_INFO *info) {
  if (!info) return E_POINTER;
  info->pFilter = _f;
  _f->AddRef();
  info->dir = PINDIR_OUTPUT;
  wcscpy(info->achName, L"Capture");
  return S_OK;
}

STDMETHODIMP Pin::EnumMediaTypes(IEnumMediaTypes **pp) {
  if (!pp) return E_POINTER;
  *pp = new EnumTypesImpl();
  return S_OK;
}

STDMETHODIMP Pin::SetFormat(AM_MEDIA_TYPE *pmt) {
  std::lock_guard<std::recursive_mutex> l(_m);
  if (!pmt) {  // back to the default
    if (_have_pref) free_mt(_pref);
    _have_pref = false;
    return S_OK;
  }
  if (!acceptable(pmt)) return VFW_E_INVALIDMEDIATYPE;
  if (_peer) {
    if (_have_mt && IsEqualGUID(_mt.subtype, pmt->subtype) && frame_time(_mt) == frame_time(*pmt)) return S_OK;
    return VFW_E_ALREADY_CONNECTED;
  }
  if (_have_pref) free_mt(_pref);
  _have_pref = copy_mt(_pref, *pmt);
  return S_OK;
}

STDMETHODIMP Pin::GetFormat(AM_MEDIA_TYPE **pmt) {
  if (!pmt) return E_POINTER;
  std::lock_guard<std::recursive_mutex> l(_m);
  if (_have_mt) *pmt = new_mt(_mt);
  else if (_have_pref) *pmt = new_mt(_pref);
  else {
    AM_MEDIA_TYPE mt;
    if (!make_type(0, mt)) return E_OUTOFMEMORY;
    *pmt = new_mt(mt);
    free_mt(mt);
  }
  return *pmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP Pin::GetStreamCaps(int i, AM_MEDIA_TYPE **pmt, BYTE *caps) {
  if (!pmt || !caps) return E_POINTER;
  if (i < 0 || i >= kTypes) return S_FALSE;
  AM_MEDIA_TYPE mt;
  if (!make_type(i, mt)) return E_OUTOFMEMORY;
  *pmt = new_mt(mt);
  const REFERENCE_TIME t = frame_time(mt);
  const LONG bits = static_cast<LONG>(reinterpret_cast<VIDEOINFOHEADER *>(mt.pbFormat)->dwBitRate);
  free_mt(mt);
  if (!*pmt) return E_OUTOFMEMORY;
  auto *c = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS *>(caps);
  std::memset(c, 0, sizeof *c);
  c->guid = kFormatVideoInfo;
  c->VideoStandard = 0;
  c->InputSize = {vcam::kOutW, vcam::kOutH};
  c->MinCroppingSize = c->MaxCroppingSize = c->InputSize;
  c->CropGranularityX = c->CropGranularityY = 1;
  c->CropAlignX = c->CropAlignY = 1;
  c->MinOutputSize = c->MaxOutputSize = c->InputSize;
  c->OutputGranularityX = c->OutputGranularityY = 1;
  c->StretchTapsX = c->StretchTapsY = 0;
  c->ShrinkTapsX = c->ShrinkTapsY = 0;
  c->MinFrameInterval = c->MaxFrameInterval = t;
  c->MinBitsPerSecond = c->MaxBitsPerSecond = bits;
  return S_OK;
}

HRESULT Pin::active() {
  std::lock_guard<std::recursive_mutex> l(_m);
  if (!_peer || !_alloc) return S_OK;  // not connected: nothing to do
  return _alloc->Commit();
}

void Pin::inactive() {
  std::lock_guard<std::recursive_mutex> l(_m);
  if (_alloc) _alloc->Decommit();
}

void Pin::run(REFERENCE_TIME start, IReferenceClock *clock) {
  stop_streaming();
  std::lock_guard<std::recursive_mutex> l(_m);
  if (!_peer || !_mip || !_alloc) return;
  _start = start;
  if (clock) clock->AddRef();
  if (_clock) _clock->Release();
  _clock = clock;
  _streaming = true;
  _t = std::thread([this] { stream(); });
}

void Pin::stop_streaming() {
  _streaming = false;
  // GetBuffer may be waiting for a free sample: decommitting the allocator releases it
  {
    std::lock_guard<std::recursive_mutex> l(_m);
    if (_t.joinable() && _alloc) _alloc->Decommit();
  }
  if (_t.joinable()) _t.join();
  std::lock_guard<std::recursive_mutex> l(_m);
  if (_alloc && _f->state() != State_Stopped) _alloc->Commit();  // paused again: ready for the next run
  if (_clock) _clock->Release();
  _clock = nullptr;
}

void Pin::stream() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMemAllocator *alloc;
  IMemInputPin *mip;
  IReferenceClock *clock;
  AM_MEDIA_TYPE mt;
  {
    std::lock_guard<std::recursive_mutex> l(_m);
    alloc = _alloc, mip = _mip, clock = _clock;
    alloc->AddRef(), mip->AddRef();
    if (clock) clock->AddRef();
    copy_mt(mt, _mt);
  }
  const bool yuy2 = IsEqualGUID(mt.subtype, kSubYUY2);
  const REFERENCE_TIME dur = frame_time(mt);
  const auto tick = std::chrono::nanoseconds(dur * 100);
  Source src(_f->slot());
  const std::wstring product = product_name();
  std::vector<uint8_t> frame, card;
  std::string pilot, card_for = "\x01";
  bool first = true;
  auto next = std::chrono::steady_clock::now();
  while (_streaming) {
    std::this_thread::sleep_until(next);
    next += tick;
    if (std::chrono::steady_clock::now() > next + tick * 4) next = std::chrono::steady_clock::now() + tick;
    IMediaSample *s = nullptr;
    if (FAILED(alloc->GetBuffer(&s, nullptr, nullptr, 0)) || !s) break;  // decommitted: stopping
    BYTE *p = nullptr;
    s->GetPointer(&p);
    const long size = s->GetSize();
    constexpr int W = vcam::kOutW, H = vcam::kOutH;
    const uint8_t *img;
    if (src.picture(frame, pilot)) {
      img = frame.data();
    } else {
      if (card_for != pilot) draw_card(_f->slot(), product, pilot, card), card_for = pilot;
      img = card.data();
    }
    if (yuy2 && size >= W * H * 2) {
      for (int y = 0; y < H; y++) {  // BT.601 limited range, two pixels per 4 bytes
        const uint8_t *r = img + static_cast<size_t>(y) * W * 4;
        uint8_t *d = p + static_cast<size_t>(y) * W * 2;
        for (int x = 0; x < W; x += 2, r += 8, d += 4) {
          const int b0 = r[0], g0 = r[1], r0 = r[2], b1 = r[4], g1 = r[5], r1 = r[6];
          d[0] = static_cast<uint8_t>((66 * r0 + 129 * g0 + 25 * b0 + 128 + 4096) >> 8);
          d[2] = static_cast<uint8_t>((66 * r1 + 129 * g1 + 25 * b1 + 128 + 4096) >> 8);
          const int rr = r0 + r1, gg = g0 + g1, bb = b0 + b1;
          d[1] = static_cast<uint8_t>(std::clamp(((-38 * rr - 74 * gg + 112 * bb) / 2 + 128) / 256 + 128, 0, 255));
          d[3] = static_cast<uint8_t>(std::clamp(((112 * rr - 94 * gg - 18 * bb) / 2 + 128) / 256 + 128, 0, 255));
        }
      }
      s->SetActualDataLength(W * H * 2);
    } else if (size >= W * H * 4) {
      for (int y = 0; y < H; y++)  // RGB32 is bottom-up
        std::memcpy(p + static_cast<size_t>(H - 1 - y) * W * 4, img + static_cast<size_t>(y) * W * 4, static_cast<size_t>(W) * 4);
      s->SetActualDataLength(W * H * 4);
    }
    s->SetSyncPoint(TRUE);
    if (first) s->SetDiscontinuity(TRUE), first = false;
    if (clock) {
      REFERENCE_TIME now = 0;
      clock->GetTime(&now);
      REFERENCE_TIME st = now - _start, en = st + dur;
      s->SetTime(&st, &en);
    } else {
      s->SetTime(nullptr, nullptr);
    }
    const HRESULT hr = mip->Receive(s);
    s->Release();
    if (hr != S_OK && hr != S_FALSE && FAILED(hr)) break;  // the downstream filter stopped taking samples
  }
  free_mt(mt);
  if (clock) clock->Release();
  mip->Release();
  alloc->Release();
  CoUninitialize();
}

// ---------------------------------------------------------------- COM plumbing
class Factory : public IClassFactory {
public:
  explicit Factory(int slot) : _slot(slot) { g_objects++; }
  ~Factory() { g_objects--; }
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *ppv = static_cast<IClassFactory *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
  STDMETHODIMP_(ULONG) Release() override {
    const long r = InterlockedDecrement(&_ref);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }
  STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    auto *f = new (std::nothrow) Filter(_slot);
    if (!f) return E_OUTOFMEMORY;
    const HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock) g_locks++;
    else g_locks--;
    return S_OK;
  }

private:
  long _ref = 1;
  int _slot;
};

std::wstring guid_text(const GUID &g) {
  wchar_t b[64];
  StringFromGUID2(g, b, 64);
  return b;
}
bool set_value(HKEY root, const std::wstring &key, const wchar_t *name, const std::wstring &value) {
  HKEY k;
  if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
  const LONG r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(k);
  return r == ERROR_SUCCESS;
}

// cmd: "<product>" (every camera) or "<product>|<mask>" (bit n: camera n; the others are removed)
HRESULT install(bool on, const std::wstring &cmd) {
  std::wstring product = cmd;
  unsigned mask = 0x1FE;  // cameras 1..8
  if (const size_t bar = cmd.find(L'|'); bar != std::wstring::npos) {
    product = cmd.substr(0, bar);
    mask = static_cast<unsigned>(wcstoul(cmd.c_str() + bar + 1, nullptr, 10));
  }
  if (product.empty()) product = L"Play League";
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_module, path, MAX_PATH);
  const std::wstring cls = L"Software\\Classes\\CLSID\\", cat = cls + guid_text(kVideoInputCategory) + L"\\Instance\\";
  bool ok = true;
  for (int n = 1; n <= vcam::kSlots; n++) {
    const std::wstring id = guid_text(camera_clsid(n)), name = product + L" Pilot " + std::to_wstring(n);
    if (on && (mask >> n & 1)) {
      ok &= set_value(HKEY_CURRENT_USER, cls + id, nullptr, name);
      ok &= set_value(HKEY_CURRENT_USER, cls + id + L"\\InprocServer32", nullptr, path);
      ok &= set_value(HKEY_CURRENT_USER, cls + id + L"\\InprocServer32", L"ThreadingModel", L"Both");
      ok &= set_value(HKEY_CURRENT_USER, cat + id, L"FriendlyName", name);
      ok &= set_value(HKEY_CURRENT_USER, cat + id, L"CLSID", id);
    } else {
      RegDeleteTreeW(HKEY_CURRENT_USER, (cls + id).c_str());
      RegDeleteTreeW(HKEY_CURRENT_USER, (cat + id).c_str());
    }
  }
  if (on) set_value(HKEY_CURRENT_USER, L"Software\\Classes\\PlayLeagueCam", L"Product", product);
  else RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\PlayLeagueCam");
  return ok ? S_OK : SELFREG_E_CLASS;
}
}  // namespace

extern "C" {
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}
__declspec(dllexport) HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv) {
  if (!ppv) return E_POINTER;
  *ppv = nullptr;
  const int n = slot_of(clsid);
  if (!n) return CLASS_E_CLASSNOTAVAILABLE;
  auto *f = new (std::nothrow) Factory(n);
  if (!f) return E_OUTOFMEMORY;
  const HRESULT hr = f->QueryInterface(riid, ppv);
  f->Release();
  return hr;
}
__declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }
__declspec(dllexport) HRESULT STDAPICALLTYPE DllInstall(BOOL on, LPCWSTR cmd) { return install(on != FALSE, cmd ? cmd : L""); }
__declspec(dllexport) HRESULT STDAPICALLTYPE DllRegisterServer() { return install(true, L"Play League"); }
__declspec(dllexport) HRESULT STDAPICALLTYPE DllUnregisterServer() { return install(false, L""); }
}
