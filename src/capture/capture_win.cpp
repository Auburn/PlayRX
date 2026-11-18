// Video capture devices through Media Foundation (see capture.h).
#include "capture.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <chrono>
#include <cstring>

namespace arx {

namespace {

template <class T>
void release(T *&p) {
  if (p) p->Release();
  p = nullptr;
}

std::string utf8(const wchar_t *w) {
  if (!w) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
std::wstring wide(const std::string &s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}
std::string attr_str(IMFActivate *a, REFGUID key) {
  WCHAR *v = nullptr;
  UINT32 len = 0;
  if (FAILED(a->GetAllocatedString(key, &v, &len))) return "";
  std::string s = utf8(v);
  CoTaskMemFree(v);
  return s;
}
int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// COM + Media Foundation for the calling thread, for as long as this lives
struct MfScope {
  bool com = false, mf = false;
  MfScope() {
    com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }
  ~MfScope() {
    if (mf) MFShutdown();
    if (com) CoUninitialize();
  }
};

std::vector<CaptureDevice> enumerate() {
  std::vector<CaptureDevice> out;
  MfScope mf;
  if (!mf.mf) return out;
  IMFAttributes *at = nullptr;
  if (FAILED(MFCreateAttributes(&at, 1))) return out;
  at->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate **devs = nullptr;
  UINT32 n = 0;
  if (SUCCEEDED(MFEnumDeviceSources(at, &devs, &n))) {
    for (UINT32 i = 0; i < n; i++) {
      CaptureDevice d;
      d.name = attr_str(devs[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
      d.id = attr_str(devs[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
      if (!d.id.empty() && d.name.rfind("Play League", 0) != 0) out.push_back(d);
      devs[i]->Release();
    }
    CoTaskMemFree(devs);
  }
  at->Release();
  return out;
}

}  // namespace

std::vector<CaptureDevice> list_capture_devices() {
  // on a thread of its own: the caller's COM apartment (WebView2's STA, ...) does not matter
  std::vector<CaptureDevice> out;
  std::thread([&] { out = enumerate(); }).join();
  return out;
}

void Capture::start(const std::string &id, Sink sink) {
  const int gen = ++_gen;
  _id = id;
  _running = true;
  _last_ms = 0;
  {
    std::lock_guard<std::mutex> l(_m);
    _error.clear();
    _format.clear();
  }
  std::thread(&Capture::run, this, id, std::move(sink), gen).detach();
}

void Capture::stop() {
  ++_gen;  // the reader ends at its next picture (or stays stuck in a dead driver, harmlessly)
  _running = false;
  _id.clear();
}

std::string Capture::error() const {
  std::lock_guard<std::mutex> l(_m);
  return _error;
}
std::string Capture::format() const {
  std::lock_guard<std::mutex> l(_m);
  return _format;
}

void Capture::run(std::string id, Sink sink, int gen) {
  MfScope mf;
  IMFAttributes *at = nullptr, *ra = nullptr;
  IMFMediaSource *src = nullptr;
  IMFSourceReader *rd = nullptr;
  IMFMediaType *best = nullptr, *out = nullptr, *cur = nullptr;
  auto mine = [&] { return _gen.load() == gen; };
  auto fail = [&](const std::string &e) {
    std::lock_guard<std::mutex> l(_m);
    if (mine()) _error = e;
  };
  do {
    if (!mf.mf) { fail("Media Foundation did not start"); break; }
    if (FAILED(MFCreateAttributes(&at, 2))) break;
    at->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    at->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, wide(id).c_str());
    if (FAILED(MFCreateDeviceSource(at, &src))) { fail("The device is not plugged in, or another program is using it"); break; }
    // decoders (MJPEG) and the colour conversion to RGB32 inside the reader
    MFCreateAttributes(&ra, 2);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (FAILED(MFCreateSourceReaderFromMediaSource(src, ra, &rd))) { fail("Could not read from the device"); break; }
    // the device's own format: the highest frame rate, then the largest picture up to 1280 x 720 (analog
    // receivers: 720 x 480 / 576 or 640 x 480)
    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    double best_score = -1;
    for (DWORD i = 0;; i++) {
      IMFMediaType *t = nullptr;
      if (FAILED(rd->GetNativeMediaType(vs, i, &t))) break;
      UINT32 w = 0, h = 0, num = 0, den = 1;
      MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
      MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den);
      const double fps = den ? static_cast<double>(num) / den : 0;
      double score = -1;
      if (w > 0 && h > 0 && w <= 1280 && h <= 720) score = std::min(fps, 60.0) * 1e7 + static_cast<double>(w) * h;
      if (score > best_score) {
        best_score = score;
        release(best);
        best = t;
      } else {
        t->Release();
      }
    }
    if (best) rd->SetCurrentMediaType(vs, nullptr, best);
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(rd->SetCurrentMediaType(vs, nullptr, out))) { fail("The device's picture format cannot be converted"); break; }
    rd->GetCurrentMediaType(vs, &cur);
    UINT32 w = 0, h = 0, num = 0, den = 1;
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(cur, MF_MT_FRAME_RATE, &num, &den);
    if (!w || !h) { fail("The device reports no picture size"); break; }
    LONG def_stride = 0;
    if (FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32 *>(&def_stride))))
      MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &def_stride);
    if (mine()) {
      std::lock_guard<std::mutex> l(_m);
      _format = std::to_string(w) + "x" + std::to_string(h) + (den && num ? " @ " + std::to_string((num + den / 2) / den) + " fps" : "");
    }
    std::vector<uint8_t> pic(static_cast<size_t>(w) * h * 4);
    while (mine()) {
      DWORD flags = 0, si = 0;
      LONGLONG ts = 0;
      IMFSample *smp = nullptr;
      const HRESULT hr = rd->ReadSample(vs, 0, &si, &flags, &ts, &smp);
      if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
        release(smp);
        fail("The device stopped (unplugged?)");
        break;
      }
      if (!smp) continue;
      IMFMediaBuffer *buf = nullptr;
      if (SUCCEEDED(smp->ConvertToContiguousBuffer(&buf))) {
        IMF2DBuffer *b2 = nullptr;
        BYTE *scan0 = nullptr;
        LONG pitch = 0;
        bool locked2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch));
        BYTE *raw = nullptr;
        DWORD len = 0;
        if (!locked2d && SUCCEEDED(buf->Lock(&raw, nullptr, &len))) {
          pitch = def_stride ? def_stride : static_cast<LONG>(w * 4);
          scan0 = pitch < 0 ? raw + static_cast<size_t>(-pitch) * (h - 1) : raw;
          if (static_cast<size_t>(std::abs(pitch)) * h > len) scan0 = nullptr;
        }
        if (scan0 && mine()) {
          const size_t row = static_cast<size_t>(w) * 4;
          for (UINT32 y = 0; y < h; y++) std::memcpy(pic.data() + y * row, scan0 + static_cast<ptrdiff_t>(y) * pitch, row);
          _last_ms = steady_ms();
          sink(pic.data(), static_cast<int>(w), static_cast<int>(h));
        }
        if (locked2d) b2->Unlock2D();
        else if (raw) buf->Unlock();
        release(b2);
        buf->Release();
      }
      smp->Release();
    }
  } while (false);
  release(cur);
  release(out);
  release(best);
  release(rd);
  if (src) src->Shutdown();
  release(src);
  release(ra);
  release(at);
  if (mine()) _running = false;
}

}  // namespace arx
