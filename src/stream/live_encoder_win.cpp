// LiveEncoder on Windows: Media Foundation's H.264 encoder used directly (an MFT), synchronous, low-latency mode,
// baseline profile (no B-frames), constant bitrate. The picture is converted from BGRX to NV12 here.
#include "live_encoder.h"

#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>

#include <algorithm>
#include <cstring>

#include "rtmp.h"  // split_annexb

namespace arx {
namespace {
const GUID kLowLatency = {STATIC_CODECAPI_AVLowLatencyMode};
const GUID kGop = {STATIC_CODECAPI_AVEncMPVGOPSize};
const GUID kRateMode = {STATIC_CODECAPI_AVEncCommonRateControlMode};
const GUID kMeanBitrate = {STATIC_CODECAPI_AVEncCommonMeanBitRate};
// ICodecAPI's interface id (MinGW has no __uuidof for it)
const IID kIID_ICodecAPI = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

template <class T> void release(T *&p) {
  if (p) p->Release();
  p = nullptr;
}
std::string hr_text(const char *what, HRESULT hr) {
  char b[96];
  std::snprintf(b, sizeof b, "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
  return b;
}
// BT.601 limited range, the analog video's own colour space
void bgrx_to_nv12(const uint8_t *src, int w, int h, uint8_t *dst) {
  uint8_t *y = dst, *uv = dst + static_cast<size_t>(w) * h;
  for (int r = 0; r < h; r++) {
    const uint8_t *s = src + static_cast<size_t>(r) * w * 4;
    uint8_t *yr = y + static_cast<size_t>(r) * w;
    for (int c = 0; c < w; c++, s += 4) yr[c] = static_cast<uint8_t>((66 * s[2] + 129 * s[1] + 25 * s[0] + 128 + 4096) >> 8);
  }
  for (int r = 0; r < h; r += 2) {
    const uint8_t *s0 = src + static_cast<size_t>(r) * w * 4, *s1 = s0 + static_cast<size_t>(w) * 4;
    uint8_t *o = uv + static_cast<size_t>(r / 2) * w;
    for (int c = 0; c < w; c += 2, s0 += 8, s1 += 8) {
      const int b = s0[0] + s0[4] + s1[0] + s1[4], g = s0[1] + s0[5] + s1[1] + s1[5], rr = s0[2] + s0[6] + s1[2] + s1[6];
      o[c] = static_cast<uint8_t>(std::clamp(((-38 * rr - 74 * g + 112 * b) / 4 + 128) / 256 + 128, 0, 255));
      o[c + 1] = static_cast<uint8_t>(std::clamp(((112 * rr - 94 * g - 18 * b) / 4 + 128) / 256 + 128, 0, 255));
    }
  }
}
}  // namespace

struct LiveEncoder::Impl {
  IMFTransform *mft = nullptr;
  DWORD out_size = 0;
  bool provides_samples = false;
  bool mf = false;
};

LiveEncoder::~LiveEncoder() { close(); }

void LiveEncoder::close() {
  if (!_m) return;
  if (_m->mft) {
    _m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    _m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  }
  release(_m->mft);
  if (_m->mf) MFShutdown();
  delete _m;
  _m = nullptr;
  sps.clear();
  pps.clear();
}

bool LiveEncoder::open(int width, int height, int fps, int kbps, int gop) {
  close();
  _err.clear();
  _w = width & ~1, _h = height & ~1, _fps = std::max(1, fps);
  _m = new Impl;
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) return _err = hr_text("MFStartup", hr), close(), false;
  _m->mf = true;
  MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12}, out{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate **acts = nullptr;
  UINT32 n = 0;
  hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &n);
  if (FAILED(hr) || n == 0) {
    if (acts) CoTaskMemFree(acts);
    return _err = "no H.264 encoder on this PC", close(), false;
  }
  hr = acts[0]->ActivateObject(IID_PPV_ARGS(&_m->mft));
  for (UINT32 i = 0; i < n; i++) acts[i]->Release();
  CoTaskMemFree(acts);
  if (FAILED(hr)) return _err = hr_text("starting the H.264 encoder", hr), close(), false;

  // settings first (some encoders only take them before the types)
  ICodecAPI *api = nullptr;
  if (SUCCEEDED(_m->mft->QueryInterface(kIID_ICodecAPI, reinterpret_cast<void **>(&api)))) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL, v.boolVal = VARIANT_TRUE;
    api->SetValue(&kLowLatency, &v);
    v.vt = VT_UI4, v.ulVal = eAVEncCommonRateControlMode_CBR;
    api->SetValue(&kRateMode, &v);
    v.vt = VT_UI4, v.ulVal = static_cast<ULONG>(kbps) * 1000;
    api->SetValue(&kMeanBitrate, &v);
    v.vt = VT_UI4, v.ulVal = static_cast<ULONG>(std::max(1, gop));
    api->SetValue(&kGop, &v);
    api->Release();
  }
  IMFMediaType *ot = nullptr, *it = nullptr;
  hr = MFCreateMediaType(&ot);
  if (SUCCEEDED(hr)) {
    ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    ot->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(kbps) * 1000);
    MFSetAttributeSize(ot, MF_MT_FRAME_SIZE, _w, _h);
    MFSetAttributeRatio(ot, MF_MT_FRAME_RATE, _fps, 1);
    MFSetAttributeRatio(ot, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);  // baseline: no B-frames, decodes anywhere
    hr = _m->mft->SetOutputType(0, ot, 0);
  }
  release(ot);
  if (FAILED(hr)) return _err = hr_text("the encoder's output format", hr), close(), false;
  hr = MFCreateMediaType(&it);
  if (SUCCEEDED(hr)) {
    it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(it, MF_MT_FRAME_SIZE, _w, _h);
    MFSetAttributeRatio(it, MF_MT_FRAME_RATE, _fps, 1);
    MFSetAttributeRatio(it, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = _m->mft->SetInputType(0, it, 0);
  }
  release(it);
  if (FAILED(hr)) return _err = hr_text("the encoder's input format", hr), close(), false;
  MFT_OUTPUT_STREAM_INFO osi{};
  _m->mft->GetOutputStreamInfo(0, &osi);
  _m->out_size = std::max<DWORD>(osi.cbSize, static_cast<DWORD>(_w) * _h * 2);
  _m->provides_samples = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
  _m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  _m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return true;
}

bool LiveEncoder::encode(const uint8_t *bgrx, int64_t t_100ns, std::vector<std::vector<uint8_t>> &nals, bool &key) {
  nals.clear();
  key = false;
  config_changed = false;
  if (!_m || !_m->mft) return false;
  // the picture in
  const DWORD len = static_cast<DWORD>(_w) * _h * 3 / 2;
  IMFMediaBuffer *buf = nullptr;
  IMFSample *in = nullptr;
  HRESULT hr = MFCreateMemoryBuffer(len, &buf);
  if (SUCCEEDED(hr)) {
    BYTE *p = nullptr;
    hr = buf->Lock(&p, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
      bgrx_to_nv12(bgrx, _w, _h, p);
      buf->Unlock();
      buf->SetCurrentLength(len);
    }
  }
  if (SUCCEEDED(hr)) hr = MFCreateSample(&in);
  if (SUCCEEDED(hr)) {
    in->AddBuffer(buf);
    in->SetSampleTime(t_100ns);
    in->SetSampleDuration(10000000LL / _fps);
  }
  release(buf);
  if (FAILED(hr)) {
    release(in);
    _err = hr_text("preparing the picture", hr);
    return false;
  }
  // the encoded picture(s) out
  auto drain = [&]() -> bool {
    for (;;) {
      MFT_OUTPUT_DATA_BUFFER ob{};
      IMFSample *os = nullptr;
      IMFMediaBuffer *obuf = nullptr;
      if (!_m->provides_samples) {
        if (FAILED(MFCreateSample(&os)) || FAILED(MFCreateMemoryBuffer(_m->out_size, &obuf))) {
          release(os), release(obuf);
          return false;
        }
        os->AddBuffer(obuf);
        release(obuf);
        ob.pSample = os;
      }
      DWORD st = 0;
      const HRESULT r = _m->mft->ProcessOutput(0, 1, &ob, &st);
      if (ob.pEvents) ob.pEvents->Release();
      if (r == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        release(os);
        return true;
      }
      if (r == MF_E_TRANSFORM_STREAM_CHANGE) {  // the encoder settled its output format: take it
        release(os);
        IMFMediaType *t = nullptr;
        if (SUCCEEDED(_m->mft->GetOutputAvailableType(0, 0, &t))) _m->mft->SetOutputType(0, t, 0), t->Release();
        continue;
      }
      if (FAILED(r)) {
        release(os);
        _err = hr_text("encoding", r);
        return false;
      }
      IMFSample *got = ob.pSample;
      IMFMediaBuffer *cb = nullptr;
      if (got && SUCCEEDED(got->ConvertToContiguousBuffer(&cb))) {
        BYTE *p = nullptr;
        DWORD n = 0;
        if (SUCCEEDED(cb->Lock(&p, nullptr, &n))) {
          for (auto &nal : split_annexb(p, n)) {
            if (nal.empty()) continue;
            const int type = nal[0] & 0x1F;
            if (type == 7) {
              if (nal != sps) sps = nal, config_changed = true;
            } else if (type == 8) {
              if (nal != pps) pps = nal, config_changed = true;
            } else if (type == 9) {
              // access unit delimiter: not wanted in FLV
            } else {
              if (type == 5) key = true;
              nals.push_back(std::move(nal));
            }
          }
          cb->Unlock();
        }
        cb->Release();
      }
      if (ob.pSample && ob.pSample != os) ob.pSample->Release();
      release(os);
    }
  };
  hr = _m->mft->ProcessInput(0, in, 0);
  if (hr == MF_E_NOTACCEPTING) {  // it still holds output: take that, then the picture
    if (!drain()) return release(in), false;
    hr = _m->mft->ProcessInput(0, in, 0);
  }
  release(in);
  if (FAILED(hr)) {
    _err = hr_text("encoding", hr);
    return false;
  }
  return drain();
}

}  // namespace arx
