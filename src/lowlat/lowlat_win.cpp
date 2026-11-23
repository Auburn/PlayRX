// AnalogRX low latency mode: the Direct3D 11 window (see lowlat.h).
//
// The window thread waits for lines (an event set every few lines by line()), uploads just the rows that changed
// into a texture, draws it letterboxed 4:3 and presents at once (sync interval 0, tearing allowed). The swap chain
// is flip model with a frame latency of 1, waited on before each frame, so presents never queue up behind the
// display: when the compositor is in the way (a window) it is one refresh behind at most; full screen, the
// display shows each present as it scans.
#include "lowlat.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "log.h"
#include "mmcss.h"

namespace arx {
namespace {

template <class T>
void release(T *&p) {
  if (p) p->Release();
  p = nullptr;
}

std::wstring wide(const std::string &s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

const char kShader[] = R"(
Texture2D pic : register(t0);
SamplerState smp : register(s0);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 ps(V i) : SV_Target { return float4(pic.Sample(smp, i.uv).rgb, 1); }
)";

constexpr wchar_t kClass[] = L"PlayLowLatency";

// Smooth lines (the decoder's build_field, one line): the line between field lines p (above) and q (below), each
// pixel averaged along whichever of 5 directions matches best, so a diagonal edge stays a clean diagonal.
void edge_interp(uint8_t *dst, const uint8_t *p, const uint8_t *q, int W) {
  auto lum = [](const uint8_t *px) { return px[0] + 2 * px[1] + px[2]; };
  for (int x = 0; x < W; x++) {
    int best = 0, bc = 1 << 30;
    for (int d = -2; d <= 2; d++) {
      const int xp = x + d, xq = x - d;
      if (xp < 0 || xp >= W || xq < 0 || xq >= W) continue;
      const int c = std::abs(lum(p + xp * 4) - lum(q + xq * 4)) + (d ? 24 + 8 * std::abs(d) : 0);  // prefer straight down
      if (c < bc) bc = c, best = d;
    }
    const uint8_t *a = p + (x + best) * 4, *b = q + (x - best) * 4;
    for (int k = 0; k < 4; k++) dst[x * 4 + k] = static_cast<uint8_t>((a[k] + b[k] + 1) >> 1);
  }
}

// the window procedure's state (one window per thread)
struct Win {
  bool resized = false, closed = false, toggle_fs = false, leave_fs = false;
};

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  Win *w = reinterpret_cast<Win *>(GetWindowLongPtrW(h, GWLP_USERDATA));
  switch (m) {
    case WM_SIZE:
      if (w) w->resized = true;
      return 0;
    case WM_LBUTTONDBLCLK:
      if (w) w->toggle_fs = true;
      return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (w && (wp == VK_F11 || (wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)))) {
        w->toggle_fs = true;
        return 0;
      }
      if (w && wp == VK_ESCAPE) {
        w->leave_fs = true;
        return 0;
      }
      break;
    case WM_CLOSE:
      if (w) w->closed = true;
      return 0;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

LowLatWindow::~LowLatWindow() { show(0, ""); }

void LowLatWindow::show(int n, const std::string &title) {
  if (n <= 0) {
    _slot = 0;
    _quit = true;
    if (_event) SetEvent(_event);
    if (_th.joinable()) _th.join();
    return;
  }
  if (!_event) _event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const int was = _slot.exchange(n);
  set_title(title);
  if (was != n) clear();
  if (_running) return;           // already open: it shows slot n from now on
  if (_th.joinable()) _th.join();     // the user closed it
  _quit = false;
  {
    std::lock_guard<std::mutex> l(_m);
    _err.clear();
    _st = Stats{};
    _title_dirty = true;
  }
  _running = true;
  _th = std::thread([this] { run(); });
}

void LowLatWindow::set_title(const std::string &title) {
  std::lock_guard<std::mutex> l(_m);
  if (_title != title) _title = title, _title_dirty = true;
}

void LowLatWindow::clear() {
  {
    std::lock_guard<std::mutex> l(_m);
    _clear_req = true;
    _pend.clear();
    for (auto &s : _stamp) std::fill(s.begin(), s.end(), 0);
  }
  if (_event) SetEvent(_event);
}

void LowLatWindow::line(int n, const kp::Line &ln) {
  if (n != _slot.load(std::memory_order_relaxed) || n == 0) return;
  bool signal = false;
  {
    std::lock_guard<std::mutex> l(_m);
    const int W = ln.width, A = ln.active, H = 2 * A;
    if (W != _w || H != _h) {
      _w = W;
      _h = H;
      for (int f = 0; f < 2; f++) {
        _fld[f].assign(static_cast<size_t>(W) * A * 4, 0);
        _stamp[f].assign(A, 0);
      }
      _pend.clear();
      _resized = true;
    }
    if (ln.row < 0 || ln.row >= A) return;
    // a new pass of a field: the other field came in between, or the lines started again from the top
    const int f = ln.field ? 1 : 0, k = ln.row;
    if (f != _lastf || k + 8 < _lastk) _pass[f]++;
    _lastf = f;
    _lastk = k;
    const size_t rowb = static_cast<size_t>(W) * 4;
    std::memcpy(_fld[f].data() + k * rowb, ln.data, rowb);
    _stamp[f][k] = _pass[f];
    _pend.push_back({f, k});
    _lines++;
    if (_pend_old == 0 && ln.t_s > 0) _pend_old = ln.t_s;
    if (++_since_signal >= 8) {  // ~0.5 ms of picture: present it
      _since_signal = 0;
      signal = true;
    }
  }
  if (signal) SetEvent(_event);
}

LowLatWindow::Stats LowLatWindow::stats() {
  std::lock_guard<std::mutex> l(_m);
  Stats s = _st;
  s.error = _err;
  return s;
}

void LowLatWindow::run() {
  auto fail = [this](const std::string &e) {
    KP_LOGE("low latency window: %s", e.c_str());
    std::lock_guard<std::mutex> l(_m);
    _err = e;
  };
  HINSTANCE inst = GetModuleHandleW(nullptr);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    registered = RegisterClassExW(&wc) != 0;
  }
  arx::mmcss_thread(true);  // the window thread ahead of ordinary threads too
  timeBeginPeriod(1);  // 1 ms timer resolution while low latency is on (sleeps and timeouts wake on time)
  Win win;
  RECT r{0, 0, 960, 720};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowExW(0, kClass, L"Low latency", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
  if (!hwnd) {
    fail("could not create the window");
    _slot = 0;
    return;
  }
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&win));
  _hwnd = hwnd;

  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  IDXGISwapChain1 *sc = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11SamplerState *smp = nullptr;
  ID3D11Texture2D *tex = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  HANDLE waitable = nullptr;
  bool tearing = false, fullscreen = false;
  UINT sc_flags = 0;
  RECT saved{};
  LONG_PTR saved_style = 0;
  int tw = 0, th = 0;
  std::vector<uint8_t> up;

  auto setup = [&]() -> bool {
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 4, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 4, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) return fail("no Direct3D 11 device"), false;
    IDXGIDevice *dd = nullptr;
    IDXGIAdapter *ad = nullptr;
    IDXGIFactory2 *f2 = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dd));
    if (dd) dd->GetAdapter(&ad);
    if (ad) ad->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&f2));
    release(ad);
    release(dd);
    if (!f2) return fail("DXGI 1.2 is missing (Windows 8 or newer needed)"), false;
    IDXGIFactory5 *f5 = nullptr;
    if (SUCCEEDED(f2->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void **>(&f5)))) {
      BOOL allow = FALSE;
      if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) tearing = allow;
      release(f5);
    }
    sc_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = sc_flags;
    hr = f2->CreateSwapChainForHwnd(dev, hwnd, &d, nullptr, nullptr, &sc);
    if (FAILED(hr)) {  // before Windows 10: no flip discard
      d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      hr = f2->CreateSwapChainForHwnd(dev, hwnd, &d, nullptr, nullptr, &sc);
    }
    f2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // full screen is ours (borderless)
    release(f2);
    if (FAILED(hr) || !sc) return fail("could not create the swap chain"), false;
    IDXGISwapChain2 *sc2 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2)))) {
      sc2->SetMaximumFrameLatency(1);
      waitable = sc2->GetFrameLatencyWaitableObject();
      release(sc2);
    }
    // the shaders, compiled here (d3dcompiler_47.dll is part of Windows 10 and newer)
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? reinterpret_cast<pD3DCompile>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) return fail("d3dcompiler_47.dll is missing"), false;
    ID3DBlob *bv = nullptr, *bp = nullptr, *err = nullptr;
    compile(kShader, sizeof(kShader) - 1, "lowlat", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &bv, &err);
    release(err);
    compile(kShader, sizeof(kShader) - 1, "lowlat", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &bp, &err);
    release(err);
    if (bv) dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &vs);
    if (bp) dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &ps);
    release(bv);
    release(bp);
    if (!vs || !ps) return fail("could not compile the shaders"), false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &smp);
    return true;
  };
  auto make_rtv = [&] {
    release(rtv);
    ID3D11Texture2D *bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb)))) {
      dev->CreateRenderTargetView(bb, nullptr, &rtv);
      release(bb);
    }
  };
  auto set_fullscreen = [&](bool on) {
    if (on == fullscreen) return;
    fullscreen = on;
    if (on) {
      GetWindowRect(hwnd, &saved);
      saved_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      MONITORINFO mi{sizeof(mi)};
      GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
      SetWindowLongPtrW(hwnd, GWL_STYLE, (saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
      SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                   mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
      ShowCursor(FALSE);
    } else {
      SetWindowLongPtrW(hwnd, GWL_STYLE, saved_style);
      SetWindowPos(hwnd, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top,
                   SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
      ShowCursor(TRUE);
    }
    win.resized = true;
  };

  const bool ok = setup();
  if (ok) {
    make_rtv();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    KP_LOGI("low latency window: open (tearing %s)", tearing ? "allowed" : "not supported: one refresh behind");
  }
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();
  uint64_t presents = 0, lines0 = 0, seen_lines = 0;
  double pend_old = 0, age_sum = 0, age_max = 0;
  uint64_t age_n = 0;
  auto last_line = clk::now();
  struct Job {
    int f, k;
    bool has[3];  // the line above / itself / below (same field, same pass)
  };
  std::vector<Job> jobs;
  std::vector<uint8_t> work, frame;  // the lines copied out; the picture (window thread only)
  bool first = true, blanked = false;
  while (ok && !_quit && !win.closed) {
    // window messages, then wait for lines (or a message, or 100 ms)
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (win.toggle_fs) win.toggle_fs = false, set_fullscreen(!fullscreen);
    if (win.leave_fs) win.leave_fs = false, set_fullscreen(false);
    if (win.closed || _quit) break;
    if (win.resized) {
      win.resized = false;
      release(rtv);
      ctx->ClearState();
      RECT cr;
      GetClientRect(hwnd, &cr);
      sc->ResizeBuffers(0, std::max<LONG>(cr.right, 1), std::max<LONG>(cr.bottom, 1), DXGI_FORMAT_UNKNOWN, sc_flags);
      make_rtv();
    } else if (!first) {
      HANDLE hs[1] = {static_cast<HANDLE>(_event)};
      MsgWaitForMultipleObjects(1, hs, FALSE, 100, QS_ALLINPUT);
    }
    // the swap chain is ready for another frame (returns at once unless the display is behind)
    if (waitable) WaitForSingleObjectEx(waitable, 50, TRUE);
    first = false;

    // the rows that changed since the last frame, into the texture
    int dlo = 1 << 30, dhi = 0;
    bool smooth = true;
    {
      std::lock_guard<std::mutex> l(_m);
      if (_clear_req) {
        _clear_req = false;
        std::fill(frame.begin(), frame.end(), 0);
        dlo = 0;
        dhi = th;
      }
      // no lines for half a second (no signal, a retune, a spare): black, not a frozen picture
      if (_lines != seen_lines) {
        seen_lines = _lines;
        last_line = clk::now();
        blanked = false;
      } else if (!blanked && clk::now() - last_line > std::chrono::milliseconds(500)) {
        std::fill(frame.begin(), frame.end(), 0);
        dlo = 0;
        dhi = th;
        blanked = true;
      }
      if (_resized || _w != tw || _h != th) {
        _resized = false;
        release(srv);
        release(tex);
        tw = _w;
        th = _h;
        if (tw > 0 && th > 0) {
          D3D11_TEXTURE2D_DESC td{};
          td.Width = tw;
          td.Height = th;
          td.MipLevels = td.ArraySize = 1;
          td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
          td.SampleDesc.Count = 1;
          td.Usage = D3D11_USAGE_DEFAULT;
          td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
          if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex))) dev->CreateShaderResourceView(tex, nullptr, &srv);
        }
        frame.assign(static_cast<size_t>(tw) * th * 4, 0);
        dlo = 0;
        dhi = th;
      }
      // the new lines, each with the lines either side of it in its field when they are from the same pass
      // (copied out, so the picture is built without holding up the decoder)
      const int A = th / 2;
      const size_t rowb = static_cast<size_t>(tw) * 4;
      jobs.clear();
      if (A > 0) {
        if (work.size() < _pend.size() * 3 * rowb) work.resize(_pend.size() * 3 * rowb);
        for (const auto &p : _pend) {
          Job j{p.f, p.k, {false, true, false}};
          uint8_t *dst = work.data() + jobs.size() * 3 * rowb;
          for (int d = -1; d <= 1; d++) {
            const int kk = p.k + d;
            const bool ok = kk >= 0 && kk < A && (d == 0 || _stamp[p.f][kk] == _pass[p.f]);
            j.has[d + 1] = ok;
            if (ok) std::memcpy(dst + (d + 1) * rowb, _fld[p.f].data() + kk * rowb, rowb);
          }
          jobs.push_back(j);
        }
      }
      _pend.clear();
      smooth = kp::smooth_lines_on();
      pend_old = _pend_old;
      _pend_old = 0;
      if (_title_dirty) {
        _title_dirty = false;
        SetWindowTextW(hwnd, wide(_title + "  —  low latency  ·  F11 / double-click: full screen").c_str());
      }
    }

    // the picture: each new line in its own place (field 1 the upper lines, 0 the lower). Smooth lines (the app's
    // setting): the line between it and its neighbour above / below in the field, filled along edges as the normal
    // picture does; off: the line twice, as the normal picture does then.
    if (tex && !jobs.empty()) {
      const int A = th / 2;
      const size_t rowb = static_cast<size_t>(tw) * 4;
      auto put = [&](int y, const uint8_t *src) {
        std::memcpy(frame.data() + y * rowb, src, rowb);
        dlo = std::min(dlo, y);
        dhi = std::max(dhi, y + 1);
      };
      auto between = [&](int y, const uint8_t *p, const uint8_t *q) {
        edge_interp(frame.data() + y * rowb, p, q, tw);
        dlo = std::min(dlo, y);
        dhi = std::max(dhi, y + 1);
      };
      for (size_t i = 0; i < jobs.size(); i++) {
        const Job &j = jobs[i];
        const uint8_t *r = work.data() + i * 3 * rowb, *above = r, *cur = r + rowb, *below = r + 2 * rowb;
        const bool upper = j.f == 1;
        const int y = upper ? 2 * j.k : 2 * j.k + 1;
        put(y, cur);
        if (!smooth) {
          put(y ^ 1, cur);
          continue;
        }
        if (y > 0) {
          if (j.has[0]) between(y - 1, above, cur);
          else if (!upper && j.k == 0) put(0, cur);
        }
        if (y + 1 < th) {
          if (j.has[2]) between(y + 1, cur, below);
          else if (upper && j.k == A - 1) put(th - 1, cur);
        }
      }
    }
    if (tex && dhi > dlo) {
      const int lo = std::max(dlo, 0), hi = std::min(dhi, th);
      if (hi > lo) {
        const D3D11_BOX box{0, static_cast<UINT>(lo), 0, static_cast<UINT>(tw), static_cast<UINT>(hi), 1};
        ctx->UpdateSubresource(tex, 0, &box, frame.data() + static_cast<size_t>(lo) * tw * 4, tw * 4, 0);
      }
    }

    // draw: the picture 4:3 in the middle, black around it
    if (rtv) {
      RECT cr;
      GetClientRect(hwnd, &cr);
      const float W = static_cast<float>(std::max<LONG>(cr.right, 1)), H = static_cast<float>(std::max<LONG>(cr.bottom, 1));
      const float pw = std::min(W, H * 4.f / 3.f), ph = pw * 3.f / 4.f;
      const float black[4] = {0, 0, 0, 1};
      ctx->OMSetRenderTargets(1, &rtv, nullptr);
      ctx->ClearRenderTargetView(rtv, black);
      if (srv) {
        const D3D11_VIEWPORT vp{(W - pw) / 2, (H - ph) / 2, pw, ph, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &smp);
        ctx->Draw(3, 0);
      }
      const HRESULT hr = sc->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
      if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        fail("the graphics device was lost: open low latency again");
        break;
      }
      presents++;
      if (pend_old > 0) {
        const double age = std::chrono::duration<double>(clk::now().time_since_epoch()).count() - pend_old;
        age_sum += age;
        age_max = std::max(age_max, age);
        age_n++;
      }
    }

    // once a second: the numbers for the UI
    const auto now = clk::now();
    const double dt = std::chrono::duration<double>(now - t0).count();
    if (dt >= 1.0) {
      std::lock_guard<std::mutex> l(_m);
      _st.tearing = tearing;
      _st.direct = fullscreen;
      _st.presents_s = static_cast<float>(presents / dt);
      _st.lines_s = static_cast<float>((_lines - lines0) / dt);
      _st.age_ms = age_n ? static_cast<float>(age_sum / age_n * 1e3) : 0.f;
      _st.age_max_ms = static_cast<float>(age_max * 1e3);
      age_sum = age_max = 0;
      age_n = 0;
      presents = 0;
      lines0 = _lines;
      t0 = now;
    }
  }
  if (fullscreen) ShowCursor(TRUE);
  if (ctx) ctx->ClearState();
  release(srv);
  release(tex);
  release(smp);
  release(ps);
  release(vs);
  release(rtv);
  if (waitable) CloseHandle(waitable);
  release(sc);
  release(ctx);
  release(dev);
  _hwnd = nullptr;
  DestroyWindow(hwnd);
  timeEndPeriod(1);
  if (win.closed || !ok) _slot = 0;  // the user closed it: the UI's button goes back to off
  _running = false;
  KP_LOGI("low latency window: closed");
}

}  // namespace arx
