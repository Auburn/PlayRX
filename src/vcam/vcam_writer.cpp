// The app's side of the virtual cameras: see vcam_writer.h.
#include "vcam_writer.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "vcam_shared.h"

namespace arx {

VCamWriter::~VCamWriter() { stop(); }

void VCamWriter::start(const std::string &product) {
  _product = product;
  for (int n = 1; n <= vcam::kSlots; n++) {
    wchar_t name[64];
    vcam::name_of(n, name, 64);
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(vcam::kBytes >> 32),
                                  static_cast<DWORD>(vcam::kBytes & 0xFFFFFFFF), name);
    if (!m) continue;
    void *v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, vcam::kBytes);
    if (!v) {
      CloseHandle(m);
      continue;
    }
    auto *h = static_cast<vcam::Header *>(v);
    h->magic = vcam::kMagic;
    h->version = vcam::kVersion;
    h->active = 0;
    _map[n] = m;
    _view[n] = v;
  }
}

bool VCamWriter::cameras(uint32_t mask) {
  mask &= 0x1FE;
  if (_applied && mask == _mask) return _err.empty();
  _mask = mask;
  _applied = true;
  _err.clear();
  // for this user (no admin needed): the DLL beside the exe registers the chosen cameras under the product's name
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::filesystem::path dll = std::filesystem::path(exe).parent_path() / L"PlayLeagueCam.dll";
  HMODULE lib = LoadLibraryW(dll.c_str());
  if (!lib) {
    _err = "PlayLeagueCam.dll not found beside the app";
    return false;
  }
  using Install = HRESULT(STDAPICALLTYPE *)(BOOL, LPCWSTR);
  auto fn = reinterpret_cast<Install>(reinterpret_cast<void *>(GetProcAddress(lib, "DllInstall")));
  const std::string cmd = _product + "|" + std::to_string(mask);
  const int wn = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
  std::wstring wp(static_cast<size_t>(std::max(wn, 1)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wp.data(), wn);
  wp.resize(wcslen(wp.c_str()));
  // none chosen: unregister them all (and the product name with them)
  const HRESULT hr = fn ? fn(mask ? TRUE : FALSE, wp.c_str()) : E_FAIL;
  FreeLibrary(lib);
  if (FAILED(hr)) _err = "the cameras could not be registered";
  return SUCCEEDED(hr);
}

void VCamWriter::stop() {
  for (int n = 1; n <= vcam::kSlots; n++) {
    if (_view[n]) {
      static_cast<vcam::Header *>(_view[n])->active = 0;
      UnmapViewOfFile(_view[n]);
    }
    if (_map[n]) CloseHandle(_map[n]);
    _view[n] = _map[n] = nullptr;
  }
}

void VCamWriter::frame(int slot, const uint8_t *bgrx, int w, int h) {
  if (slot < 1 || slot > vcam::kSlots || !_view[slot] || w <= 0 || h <= 0 || w > vcam::kMaxW || h > vcam::kMaxH) return;
  auto *hd = static_cast<vcam::Header *>(_view[slot]);
  InterlockedIncrement(&hd->seq);  // odd: being written
  hd->width = static_cast<uint32_t>(w);
  hd->height = static_cast<uint32_t>(h);
  std::memcpy(reinterpret_cast<uint8_t *>(hd + 1), bgrx, static_cast<size_t>(w) * h * 4);
  hd->written_ms = static_cast<int64_t>(GetTickCount64());
  InterlockedIncrement(&hd->seq);  // even: whole
}

void VCamWriter::set_slot(int slot, bool active, const std::string &pilot) {
  if (slot < 1 || slot > vcam::kSlots || !_view[slot]) return;
  auto *hd = static_cast<vcam::Header *>(_view[slot]);
  std::memset(hd->name, 0, sizeof hd->name);
  std::memcpy(hd->name, pilot.data(), std::min(pilot.size(), sizeof hd->name - 1));
  hd->active = active ? 1 : 0;
}

}  // namespace arx
