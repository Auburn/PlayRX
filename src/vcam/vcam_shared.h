// Virtual cameras (Windows): how Play League / Play RX hand each pilot's picture to the camera DLL
// (PlayLeagueCam.dll, which runs inside the program that opens the camera: FPV Trackside, OBS, ...).
// One named shared memory block per pilot slot, "Local\PlayLeagueVCam<N>": a small header, then the newest
// picture (B, G, R, x rows, top first, the decoder's own size, at most kMaxW x kMaxH). A sequence number makes it
// safe without locks: odd while the writer copies, even when the picture is whole (the reader copies and checks
// the number did not move).
#pragma once
#include <cstdint>

namespace arx::vcam {

constexpr uint32_t kMagic = 0x43564C50;  // "PLVC"
constexpr uint32_t kVersion = 1;
constexpr int kSlots = 8;
constexpr int kMaxW = 768, kMaxH = 576;  // PAL 720 x 576 with room to spare
constexpr int kOutW = 640, kOutH = 480;  // what the cameras give: 4:3, square pixels

struct Header {
  uint32_t magic, version;
  uint32_t width, height;  // of the picture now in the buffer
  volatile long seq;       // odd: being written
  uint32_t active;         // a pilot is in the slot (0: show "no video")
  int64_t written_ms;      // GetTickCount64() of the last picture: stale ones show "no video"
  char name[48];           // the pilot (UTF-8), for the "no video" card
  uint32_t reserved[8];
};
constexpr uint64_t kBytes = sizeof(Header) + static_cast<uint64_t>(kMaxW) * kMaxH * 4;

inline void name_of(int slot, wchar_t *out, int n) {  // "Local\PlayLeagueVCam3"
  const wchar_t *p = L"Local\\PlayLeagueVCam";
  int i = 0;
  while (*p && i < n - 3) out[i++] = *p++;
  out[i++] = static_cast<wchar_t>(L'0' + slot);
  out[i] = 0;
}

}  // namespace arx::vcam
