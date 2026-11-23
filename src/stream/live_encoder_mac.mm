// Live H.264 for streaming on macOS: VideoToolbox (the Mac's hardware encoder), real-time, no B-frames, so each
// picture comes straight back (see live_encoder.h). Output is turned from VideoToolbox's length-prefixed NAL units
// into plain NAL units; SPS / PPS come from the format description.
#include "live_encoder.h"

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstring>

namespace arx {

namespace {

// what the output callback fills (it runs before VTCompressionSessionCompleteFrames returns)
struct VtState {
  VTCompressionSessionRef vt = nullptr;
  std::vector<std::vector<uint8_t>> nals;
  std::vector<uint8_t> sps, pps;
  bool key = false;
  OSStatus status = noErr;
};

void on_output(void *ref, void *, OSStatus st, VTEncodeInfoFlags, CMSampleBufferRef sb) {
  auto *m = static_cast<VtState *>(ref);
  if (st != noErr) {
    m->status = st;
    return;
  }
  if (!sb || !CMSampleBufferDataIsReady(sb)) return;
  // a key frame has no "not sync" attachment
  bool key = true;
  if (CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false); att && CFArrayGetCount(att) > 0) {
    auto d = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(att, 0));
    CFBooleanRef ns = nullptr;
    if (CFDictionaryGetValueIfPresent(d, kCMSampleAttachmentKey_NotSync, reinterpret_cast<const void **>(&ns)) && ns &&
        CFBooleanGetValue(ns))
      key = false;
  }
  if (key) {
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
    const uint8_t *p = nullptr;
    size_t n = 0, count = 0;
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, &p, &n, &count, nullptr) == noErr && p)
      m->sps.assign(p, p + n);
    if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 1, &p, &n, &count, nullptr) == noErr && p)
      m->pps.assign(p, p + n);
  }
  m->key = m->key || key;
  CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
  size_t here = 0, total = 0;
  char *data = nullptr;
  if (!bb || CMBlockBufferGetDataPointer(bb, 0, &here, &total, &data) != noErr || !data) return;
  std::vector<char> whole;  // in pieces (rare): copy it into one
  if (here < total) {
    whole.resize(total);
    if (CMBlockBufferCopyDataBytes(bb, 0, total, whole.data()) != kCMBlockBufferNoErr) return;
    data = whole.data();
  }
  // 4-byte big-endian lengths, then the NAL unit
  size_t i = 0;
  while (i + 4 <= total) {
    const auto *u = reinterpret_cast<const uint8_t *>(data + i);
    const size_t len = (size_t(u[0]) << 24) | (size_t(u[1]) << 16) | (size_t(u[2]) << 8) | u[3];
    i += 4;
    if (len == 0 || i + len > total) break;
    const int type = u[4] & 0x1F;
    if (type != 7 && type != 8 && type != 9 && type != 6)  // parameter sets go separately; no delimiters / SEI in FLV
      m->nals.emplace_back(u + 4, u + 4 + len);
    i += len;
  }
}

void set_num(VTCompressionSessionRef s, CFStringRef k, int v) {
  CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &v);
  VTSessionSetProperty(s, k, n);
  CFRelease(n);
}

std::string os_text(const char *what, OSStatus st) { return std::string(what) + " failed (" + std::to_string(st) + ")"; }

}  // namespace

struct LiveEncoder::Impl : VtState {};

LiveEncoder::~LiveEncoder() { close(); }

bool LiveEncoder::open(int width, int height, int fps, int kbps, int gop) {
  close();
  _w = width, _h = height, _fps = fps > 0 ? fps : 30;
  _m = new Impl;
  sps.clear(), pps.clear();
  // hardware, low-latency rate control (macOS 11.3+) when there is one; otherwise whatever VideoToolbox offers
  const void *keys[] = {kVTVideoEncoderSpecification_EnableLowLatencyRateControl};
  const void *vals[] = {kCFBooleanTrue};
  CFDictionaryRef spec = CFDictionaryCreate(nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  const void *pk[] = {kCVPixelBufferPixelFormatTypeKey};
  int fmt = kCVPixelFormatType_32BGRA;
  CFNumberRef fmtn = CFNumberCreate(nullptr, kCFNumberIntType, &fmt);
  const void *pv[] = {fmtn};
  CFDictionaryRef attrs = CFDictionaryCreate(nullptr, pk, pv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  OSStatus st = VTCompressionSessionCreate(nullptr, width, height, kCMVideoCodecType_H264, spec, attrs, nullptr, on_output, static_cast<VtState *>(_m), &_m->vt);
  if (st != noErr) st = VTCompressionSessionCreate(nullptr, width, height, kCMVideoCodecType_H264, nullptr, attrs, nullptr, on_output, static_cast<VtState *>(_m), &_m->vt);
  CFRelease(spec), CFRelease(attrs), CFRelease(fmtn);
  if (st != noErr || !_m->vt) {
    _err = os_text("the H.264 encoder", st);
    close();
    return false;
  }
  VTCompressionSessionRef s = _m->vt;
  VTSessionSetProperty(s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty(s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Baseline_AutoLevel);
  set_num(s, kVTCompressionPropertyKey_AverageBitRate, kbps * 1000);
  set_num(s, kVTCompressionPropertyKey_ExpectedFrameRate, _fps);
  set_num(s, kVTCompressionPropertyKey_MaxKeyFrameInterval, gop > 0 ? gop : _fps);
  VTCompressionSessionPrepareToEncodeFrames(s);
  return true;
}

bool LiveEncoder::encode(const uint8_t *bgrx, int64_t t_100ns, std::vector<std::vector<uint8_t>> &nals, bool &key) {
  nals.clear();
  key = false;
  config_changed = false;
  if (!_m || !_m->vt) return false;
  CVPixelBufferRef pb = nullptr;
  CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(_m->vt);
  if (!pool || CVPixelBufferPoolCreatePixelBuffer(nullptr, pool, &pb) != kCVReturnSuccess || !pb) {
    _err = "no picture buffer from the encoder";
    return false;
  }
  CVPixelBufferLockBaseAddress(pb, 0);
  auto *dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(pb));
  const size_t stride = CVPixelBufferGetBytesPerRow(pb), row = static_cast<size_t>(_w) * 4;
  for (int y = 0; y < _h; y++) std::memcpy(dst + y * stride, bgrx + y * row, row);
  CVPixelBufferUnlockBaseAddress(pb, 0);
  _m->nals.clear();
  _m->key = false;
  _m->status = noErr;
  const CMTime pts = CMTimeMake(t_100ns, 10000000);
  OSStatus st = VTCompressionSessionEncodeFrame(_m->vt, pb, pts, CMTimeMake(1, _fps), nullptr, nullptr, nullptr);
  CVPixelBufferRelease(pb);
  if (st == noErr) st = VTCompressionSessionCompleteFrames(_m->vt, pts);  // the picture out now (no lookahead)
  if (st == noErr) st = _m->status;
  if (st != noErr) {
    _err = os_text("encoding", st);
    return false;
  }
  if (!_m->sps.empty() && _m->sps != sps) sps = _m->sps, config_changed = true;
  if (!_m->pps.empty() && _m->pps != pps) pps = _m->pps, config_changed = true;
  nals.swap(_m->nals);
  key = _m->key;
  return true;
}

void LiveEncoder::close() {
  if (!_m) return;
  if (_m->vt) {
    VTCompressionSessionCompleteFrames(_m->vt, kCMTimeInvalid);
    VTCompressionSessionInvalidate(_m->vt);
    CFRelease(_m->vt);
  }
  delete _m;
  _m = nullptr;
}

}  // namespace arx
