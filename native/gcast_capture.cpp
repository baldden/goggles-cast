// gcast_capture: the screen -> the GPU's H.264 encoder -> DJI Goggles 2 / Integra on this PC's USB, for Goggles Cast.
// Capture and encoding are ported from Apollo (github.com/ClassicOldSong/Apollo, GPL-3.0, LICENSE.apollo), the
// Sunshine fork with the lowest latency, not re-invented:
//   - capture: DXGI Desktop Duplication, copy into an own texture and release the frame at once (display_base.cpp /
//     display_vram.cpp)
//   - cursor: the pointer shape split into alpha and XOR images and blended with Apollo's cursor shaders and blend
//     states (make_cursor_alpha_image / make_cursor_xor_image / make_blend / blend_cursor)
//   - BGRA -> NV12 on the GPU with Apollo's shaders (convert_yuv420_planar_y / convert_yuv420_packed_uv_type0) and
//     colour matrix (video_colorspace.cpp, BT.709 limited), straight into the encoder's own texture
//   - the encoder's texture: libavcodec D3D11 frames with D3D11_BIND_RENDER_TARGET, pool of one (init_hwframes)
//   - encoder options: Apollo's (video.cpp amdvce / quicksync, nvenc/nvenc_base.cpp with config.cpp's defaults), a
//     VBV of one frame, no filler; intra refresh and a key frame every few seconds for the goggles
// The frames go to the goggles over USB (goggles_usb.h).
//
// stdin: the byte 'I' asks for a key frame; closing it ends the program.
// stderr: one status line per second ("stat ...") and the USB link's lines ("usb: ...").
//
// Usage: gcast_capture --display \\.\DISPLAY1 --width 1920 --height 1080 --fps 60 --bitrate 24 [--vbv-frames 1]
//        [--gop 180] [--encoder auto|amf|nvenc|qsv] [--cursor 1] [--quality speed] [--shaders DIR]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <io.h>
#include <fcntl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
}

template <class T> struct Com {
  T *p = nullptr;
  ~Com() { reset(); }
  void reset() { if (p) { p->Release(); p = nullptr; } }
  T **put() { reset(); return &p; }
  T *operator->() const { return p; }
  operator T *() const { return p; }
};

static void die(const char *what, long code = 0) {
  fprintf(stderr, "error %s 0x%08lx\n", what, (unsigned long) code);
  fflush(stderr);
  exit(2);
}

static int64_t qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

struct Args {
  std::wstring display = L"";
  int width = 1920, height = 1080, fps = 60, gop = 0;
  double bitrate = 50, vbv_frames = 3;
  std::string encoder = "auto", quality = "speed";
  bool cursor = true;
  double min_fps = 0;   // --min-fps: Apollo's minimum_fps_target (0 = its default, fps / 5, at least 10)
  std::wstring shaders;
};

static Args parse(int argc, wchar_t **argv) {
  Args a;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::wstring k = argv[i], v = argv[i + 1];
    std::string vs(v.begin(), v.end());
    if (k == L"--display") a.display = v;
    else if (k == L"--width") a.width = _wtoi(v.c_str());
    else if (k == L"--height") a.height = _wtoi(v.c_str());
    else if (k == L"--fps") a.fps = _wtoi(v.c_str());
    else if (k == L"--gop") a.gop = _wtoi(v.c_str());
    else if (k == L"--bitrate") a.bitrate = _wtof(v.c_str());
    else if (k == L"--vbv-frames") a.vbv_frames = _wtof(v.c_str());
    else if (k == L"--encoder") a.encoder = vs;
    else if (k == L"--quality") a.quality = vs;
    else if (k == L"--cursor") a.cursor = v != L"0";
    else if (k == L"--min-fps") a.min_fps = _wtof(v.c_str());
    else if (k == L"--shaders") a.shaders = v;
  }
  return a;
}

// ---- Apollo: video_colorspace.cpp, color_vectors_from_colorspace (BT.709 MPEG range) -----------------------------
struct ColorT { float y[4], u[4], v[4], range_y[2], range_uv[2]; };
static ColorT bt709_limited() {
  const float Cr = 0.2126f, Cb = 0.0722f, Cg = 1.0f - Cr - Cb, Cr_i = 1.0f - Cr, Cb_i = 1.0f - Cb;
  const float ry[2] = {16.0f, 235.0f}, ruv[2] = {16.0f, 240.0f};
  return {{Cr, Cg, Cb, 0.0f},
          {-(Cr * 0.5f / Cb_i), -(Cg * 0.5f / Cb_i), 0.5f, 0.5f},
          {0.5f, -(Cg * 0.5f / Cr_i), -(Cb * 0.5f / Cr_i), 0.5f},
          {(ry[1] - ry[0]) / 255.0f, ry[0] / 255.0f},
          {(ruv[1] - ruv[0]) / 255.0f, ruv[0] / 255.0f}};
}

// ---- Apollo: display_vram.cpp, make_cursor_alpha_image / make_cursor_xor_image -------------------------------------
static std::vector<uint8_t> cursor_xor_image(const std::vector<uint8_t> &img, DXGI_OUTDUPL_POINTER_SHAPE_INFO si) {
  const uint32_t inverted = 0xFFFFFFFF, transparent = 0;
  if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) return {};
  if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
    std::vector<uint8_t> out = img;
    for (size_t i = 0; i + 3 < out.size(); i += 4) {
      uint32_t &px = *(uint32_t *) &out[i];
      if (((px >> 24) & 0xFF) == 0x00) px = transparent;
    }
    return out;
  }
  si.Height /= 2;
  std::vector<uint8_t> out(si.Width * si.Height * 4);
  uint32_t *pd = (uint32_t *) out.data(), *end = (uint32_t *) (out.data() + out.size());
  const uint8_t *and_mask = img.data(), *xor_mask = img.data() + si.Pitch * si.Height;
  for (UINT x = 0; x < si.Pitch * si.Height; ++x, ++and_mask, ++xor_mask)
    for (int c = 7; c >= 0 && pd != end; --c, ++pd) {
      int bit = 1 << c, t = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);
      *pd = t == 3 ? inverted : transparent;
    }
  return out;
}

static std::vector<uint8_t> cursor_alpha_image(const std::vector<uint8_t> &img, DXGI_OUTDUPL_POINTER_SHAPE_INFO si) {
  const uint32_t black = 0xFF000000, white = 0xFFFFFFFF, transparent = 0;
  if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) return img;
  if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
    std::vector<uint8_t> out = img;
    for (size_t i = 0; i + 3 < out.size(); i += 4) {
      uint32_t &px = *(uint32_t *) &out[i];
      uint8_t a = (px >> 24) & 0xFF;
      if (a == 0xFF) px = transparent;
      else if (a == 0x00) px |= 0xFF000000;
    }
    return out;
  }
  si.Height /= 2;
  std::vector<uint8_t> out(si.Width * si.Height * 4);
  uint32_t *pd = (uint32_t *) out.data(), *end = (uint32_t *) (out.data() + out.size());
  const uint8_t *and_mask = img.data(), *xor_mask = img.data() + si.Pitch * si.Height;
  for (UINT x = 0; x < si.Pitch * si.Height; ++x, ++and_mask, ++xor_mask)
    for (int c = 7; c >= 0 && pd != end; --c, ++pd) {
      int bit = 1 << c, t = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);
      *pd = t == 0 ? black : t == 2 ? white : transparent;
    }
  return out;
}

// ---- D3D helpers ---------------------------------------------------------------------------------------------------
static ID3DBlob *compile(const std::wstring &dir, const wchar_t *file, const char *entry, const char *model) {
  ID3DBlob *code = nullptr, *msg = nullptr;
  std::wstring path = dir + L"\\" + file;
  HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, model,
                                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &msg);
  if (FAILED(hr)) {
    fwprintf(stderr, L"error shader %ls: %hs\n", file, msg ? (const char *) msg->GetBufferPointer() : "?");
    exit(2);
  }
  if (msg) msg->Release();
  return code;
}

static ID3D11Buffer *make_cbuffer(ID3D11Device *dev, const void *data, UINT size) {
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = (size + 15) / 16 * 16;
  d.Usage = D3D11_USAGE_IMMUTABLE;
  d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  std::vector<uint8_t> padded(d.ByteWidth, 0);
  memcpy(padded.data(), data, size);
  D3D11_SUBRESOURCE_DATA init{padded.data(), 0, 0};
  ID3D11Buffer *b = nullptr;
  if (FAILED(dev->CreateBuffer(&d, &init, &b))) die("cbuffer");
  return b;
}

// Apollo: make_blend
static ID3D11BlendState *make_blend(ID3D11Device *dev, bool enable, bool invert) {
  D3D11_BLEND_DESC bd{};
  auto &rt = bd.RenderTarget[0];
  rt.BlendEnable = enable;
  rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (enable) {
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.SrcBlend = invert ? D3D11_BLEND_INV_DEST_COLOR : D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = invert ? D3D11_BLEND_INV_SRC_COLOR : D3D11_BLEND_INV_SRC_ALPHA;
    rt.SrcBlendAlpha = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
  }
  ID3D11BlendState *b = nullptr;
  if (FAILED(dev->CreateBlendState(&bd, &b))) die("blend");
  return b;
}

struct CursorTex {
  Com<ID3D11Texture2D> tex;
  Com<ID3D11ShaderResourceView> srv;
  UINT w = 0, h = 0;
};

static void make_cursor_tex(ID3D11Device *dev, CursorTex &c, const std::vector<uint8_t> &img, UINT w, UINT h) {
  c.tex.reset(); c.srv.reset();
  if (img.empty()) return;
  D3D11_TEXTURE2D_DESC t{};
  t.Width = w; t.Height = h; t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_IMMUTABLE; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{img.data(), w * 4, 0};
  if (FAILED(dev->CreateTexture2D(&t, &init, c.tex.put()))) return;
  dev->CreateShaderResourceView(c.tex, nullptr, c.srv.put());
  c.w = w; c.h = h;
}

static std::atomic<bool> g_want_key{false};
// where frames could go missing, counted by the capture thread, printed by the encoder thread once a second
static std::atomic<int64_t> g_acc_lost{0}, g_late{0}, g_late_tick{0}, g_repeats{0}, g_ptr{0};
static std::atomic<int64_t> g_game_replaced{0};   // game pictures replaced before their tick took them (capture -> pacer)
static const bool g_trace = getenv("GCAST_TRACE") != nullptr;   // per-frame debug lines on stderr
static std::atomic<int64_t> g_presents{0};   // new pictures Windows showed on the monitor = the game's own frame rate

// ---- to the goggles ------------------------------------------------------------------------------------------
static int64_t g_freq = 1;
static uint8_t g_crc8[256];
static uint16_t g_crc16[256];
static void crc_tables() {
  for (int i = 0; i < 256; ++i) {
    uint32_t a = i, b = i;
    for (int k = 0; k < 8; ++k) {
      a = (a & 1) ? (a >> 1) ^ 0x8C : a >> 1;           // duml.py crc8 (init 0x77)
      b = (b & 1) ? (b >> 1) ^ 0x8408 : b >> 1;         // duml.py crc16 (init 0x3692)
    }
    g_crc8[i] = (uint8_t) a; g_crc16[i] = (uint16_t) b;
  }
}
static uint8_t crc8(const uint8_t *d, size_t n) { uint8_t c = 0x77; while (n--) c = g_crc8[c ^ *d++]; return c; }
static uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0x3692;
  while (n--) c = g_crc16[(c ^ *d++) & 0xFF] ^ (c >> 8);
  return c;
}
template <class T> static void put_le(uint8_t *p, T v) { for (size_t i = 0; i < sizeof(T); ++i) p[i] = uint8_t(uint64_t(v) >> (8 * i)); }

struct VPacket { std::vector<uint8_t> au; bool idr; int64_t t_present, t_cap, t_enc; };

// Every key frame goes out with SPS/PPS in front, as DJI's Virtual Flight app sends them (a KEY_FRAME buffer is sent
// as SPS/PPS + the frame). A forced key frame without them is what left the goggles black in
// 0.8.21 ("non-existing PPS 0 referenced"): the last ones the encoder gave are put in front when it leaves them out.
// With intra refresh there is no key frame by the clock, so SPS/PPS also go before a frame every refresh period
// (g_gop frames): a decoder that starts in the middle of the stream gets them within that time.
static std::vector<uint8_t> g_sps_pps;
static int g_gop = 0, g_since_headers = 0;
static void key_frame_headers(std::vector<uint8_t> &au, bool idr) {
  std::vector<uint8_t> hdr;
  bool has_sps = false;
  size_t i = 0;
  auto next_start = [&](size_t from) {   // the next 00 00 01 at or after from, au.size() if none
    for (size_t k = from; k + 2 < au.size(); ++k)
      if (au[k] == 0 && au[k + 1] == 0 && au[k + 2] == 1) return k;
    return au.size();
  };
  i = next_start(0);
  while (i < au.size()) {
    const size_t s = i + 3, e0 = next_start(s);
    size_t e = e0;
    while (e > s && e < au.size() && au[e - 1] == 0) --e;   // a 4-byte start code belongs to the next NAL
    const int type = s < au.size() ? au[s] & 0x1F : 0;
    if (type == 7 || type == 8) {
      if (type == 7) has_sps = true;
      static const uint8_t sc[4] = {0, 0, 0, 1};
      hdr.insert(hdr.end(), sc, sc + 4);
      hdr.insert(hdr.end(), au.begin() + s, au.begin() + e);
    }
    i = e0;
  }
  if (has_sps) {
    g_sps_pps = hdr;
    g_since_headers = 0;
  } else if (!g_sps_pps.empty() && (idr || (g_gop > 0 && ++g_since_headers >= g_gop))) {
    au.insert(au.begin(), g_sps_pps.begin(), g_sps_pps.end());
    g_since_headers = 0;
  }
}
static std::mutex g_vq_m;
static std::condition_variable g_vq_cv;
static std::deque<VPacket> g_vq;   // Apollo's mail::video_packets: safe::queue_t, 32 at most, cleared when full

#include "goggles_usb.h"
static usbg::Link *g_usb = nullptr;

// The encoded frames to the goggles, one at a time, in their order (Apollo's video_packets queue -> its sender)
static void sender_thread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // adjust_thread_priority(high)
  for (;;) {
    VPacket p;
    {
      std::unique_lock<std::mutex> l(g_vq_m);
      g_vq_cv.wait(l, [] { return !g_vq.empty(); });
      p = std::move(g_vq.front());
      g_vq.pop_front();
    }
    g_usb->send_frame(p.au.data(), p.au.size(), p.idr);   // a frame too big for the goggles asks for a key frame
  }
}

int wmain(int argc, wchar_t **argv) {
  Args a = parse(argc, argv);
  timeBeginPeriod(1);   // 1 ms timer: the repeat timeouts (10 ms at 100 Hz) are 15.6 ms steps otherwise
  _setmode(_fileno(stdin), _O_BINARY);
  if (a.shaders.empty()) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    a.shaders = exe;
    a.shaders = a.shaders.substr(0, a.shaders.find_last_of(L"\\/")) + L"\\shaders";
  }
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);   // like Sunshine: never wait behind the game
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

  // ---- the adapter and output that show the display ----
  Com<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) factory.put()))) die("dxgi factory");
  Com<IDXGIAdapter1> adapter;
  Com<IDXGIOutput> output;
  for (UINT ai = 0; !output && factory->EnumAdapters1(ai, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++ai) {
    Com<IDXGIOutput> o;
    for (UINT oi = 0; adapter->EnumOutputs(oi, o.put()) != DXGI_ERROR_NOT_FOUND; ++oi) {
      DXGI_OUTPUT_DESC d;
      o->GetDesc(&d);
      if (!d.AttachedToDesktop) continue;
      if (a.display.empty() || a.display == d.DeviceName) {
        output.p = o.p; o.p = nullptr;
        fwprintf(stderr, L"display %ls %ldx%ld\n", d.DeviceName, d.DesktopCoordinates.right - d.DesktopCoordinates.left,
                 d.DesktopCoordinates.bottom - d.DesktopCoordinates.top);
        break;
      }
    }
    if (!output) adapter.reset();
  }
  if (!output) die("display not found");

  Com<ID3D11Device> dev;
  Com<ID3D11DeviceContext> ctx;
  D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, fl, 2,
                               D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put())))
    die("d3d11 device");
  {
    Com<ID3D10Multithread> mt;   // libavcodec's encoders may touch the context from their own thread
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), (void **) mt.put()))) mt->SetMultithreadProtected(TRUE);
  }
  {
    Com<IDXGIDevice1> dxdev;     // Sunshine: lowest GPU latency for this process
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice1), (void **) dxdev.put()))) {
      dxdev->SetMaximumFrameLatency(1);
      dxdev->SetGPUThreadPriority(7);
    }
  }

  // The encoder gets its own device, like Apollo: Desktop Duplication's AcquireNextFrame holds the capture device
  // while it waits for the next picture, and an encoder on the same device then waits a whole frame (measured
  // 2026-10-19: screen -> encoded 21 ms at 60 Hz instead of 5). Pictures go across in shared, keyed textures.
  Com<ID3D11Device> edev;
  Com<ID3D11DeviceContext> ectx;
  if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, fl, 2,
                               D3D11_SDK_VERSION, edev.put(), nullptr, ectx.put())))
    die("d3d11 encoder device");
  {
    Com<ID3D10Multithread> mt;
    if (SUCCEEDED(edev->QueryInterface(__uuidof(ID3D10Multithread), (void **) mt.put()))) mt->SetMultithreadProtected(TRUE);
    Com<IDXGIDevice1> dxdev;
    if (SUCCEEDED(edev->QueryInterface(__uuidof(IDXGIDevice1), (void **) dxdev.put()))) {
      dxdev->SetMaximumFrameLatency(1);
      dxdev->SetGPUThreadPriority(7);
    }
  }

  Com<IDXGIOutput1> out1;
  if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void **) out1.put()))) die("output1");
  Com<IDXGIOutputDuplication> dup;
  auto open_dup = [&]() -> HRESULT {
    dup.reset();
    Com<IDXGIOutput5> out5;
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput5), (void **) out5.put()))) {
      DXGI_FORMAT fmts[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
      HRESULT hr = out5->DuplicateOutput1(dev, 0, 1, fmts, dup.put());
      if (SUCCEEDED(hr)) return hr;
    }
    return out1->DuplicateOutput(dev, dup.put());
  };
  if (FAILED(open_dup())) die("duplicate output");
  DXGI_OUTDUPL_DESC ddesc;
  dup->GetDesc(&ddesc);
  const UINT sw = ddesc.ModeDesc.Width, sh = ddesc.ModeDesc.Height;

  // ---- own capture texture (BGRA, the cursor is drawn into it) ----
  Com<ID3D11Texture2D> cap;
  Com<ID3D11ShaderResourceView> cap_srv;
  Com<ID3D11RenderTargetView> cap_rtv;
  {
    D3D11_TEXTURE2D_DESC t{};
    t.Width = sw; t.Height = sh; t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&t, nullptr, cap.put()))) die("capture texture");
    dev->CreateShaderResourceView(cap, nullptr, cap_srv.put());
    dev->CreateRenderTargetView(cap, nullptr, cap_rtv.put());
  }

  // ---- shaders (Apollo's files, compiled here like Apollo does) ----
  Com<ID3D11VertexShader> vs_y, vs_uv, vs_cursor;
  Com<ID3D11PixelShader> ps_y, ps_uv, ps_cursor;
  {
    ID3DBlob *b;
    b = compile(a.shaders, L"convert_yuv420_planar_y_vs.hlsl", "main_vs", "vs_5_0");
    edev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, vs_y.put()); b->Release();
    b = compile(a.shaders, L"convert_yuv420_planar_y_ps.hlsl", "main_ps", "ps_5_0");
    edev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, ps_y.put()); b->Release();
    b = compile(a.shaders, L"convert_yuv420_packed_uv_type0_vs.hlsl", "main_vs", "vs_5_0");
    edev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, vs_uv.put()); b->Release();
    b = compile(a.shaders, L"convert_yuv420_packed_uv_type0_ps.hlsl", "main_ps", "ps_5_0");
    edev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, ps_uv.put()); b->Release();
    b = compile(a.shaders, L"cursor_vs.hlsl", "main_vs", "vs_5_0");
    dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, vs_cursor.put()); b->Release();
    b = compile(a.shaders, L"cursor_ps.hlsl", "main_ps", "ps_5_0");
    dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, ps_cursor.put()); b->Release();
  }
  ColorT color = bt709_limited();
  Com<ID3D11Buffer> cb_color, cb_subsample, cb_rotate;
  cb_color.p = make_cbuffer(edev, &color, sizeof color);
  float subsample[4] = {1.0f / a.width, 1.0f / a.height, 0, 0};
  cb_subsample.p = make_cbuffer(edev, subsample, sizeof subsample);
  int32_t rotate[4] = {0, 0, 0, 0};
  cb_rotate.p = make_cbuffer(dev, rotate, sizeof rotate);
  Com<ID3D11Buffer> cb_rotate_e;
  cb_rotate_e.p = make_cbuffer(edev, rotate, sizeof rotate);
  Com<ID3D11SamplerState> sampler, sampler_e;
  {
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.ComparisonFunc = D3D11_COMPARISON_NEVER;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&s, sampler.put()))) die("sampler");
    if (FAILED(edev->CreateSamplerState(&s, sampler_e.put()))) die("sampler");
  }
  Com<ID3D11BlendState> blend_alpha, blend_invert, blend_disable;
  blend_alpha.p = make_blend(dev, true, false);
  blend_invert.p = make_blend(dev, true, true);
  blend_disable.p = make_blend(dev, false, false);
  Com<ID3D11BlendState> blend_disable_e;
  blend_disable_e.p = make_blend(edev, false, false);

  // ---- encoder (libavcodec, the encoder's own D3D11 texture) ----
  std::vector<std::string> order;
  if (a.encoder == "auto") order = {"h264_nvenc", "h264_amf", "h264_qsv"};
  else order = {a.encoder == "amf" ? "h264_amf" : a.encoder == "nvenc" ? "h264_nvenc" : a.encoder == "qsv" ? "h264_qsv"
                                                                                       : a.encoder};
  AVBufferRef *hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  {
    auto *dctx = (AVD3D11VADeviceContext *) ((AVHWDeviceContext *) hwdev->data)->hwctx;
    edev->AddRef();
    dctx->device = edev;
    if (av_hwdevice_ctx_init(hwdev) < 0) die("av hwdevice");
  }
  AVCodecContext *enc = nullptr;
  std::string enc_name;
  const int64_t b = (int64_t) (a.bitrate * 1e6);
  // The periodic refresh: Apollo's intra refresh (nvenc/nvenc_base.cpp enableIntraRefresh), not a key frame by the
  // clock. A key frame every a.gop frames (0.8.22-0.8.24) in a VBV of one frame is a key frame of one frame's size: it
  // came out blurred and the next frames sharpened again, the picture pulsed once a second (the user saw it on a still
  // desktop; measured 2026-10-30, AMF 50 Mbit/s 60 Hz, still desktop: every key frame 104.9 KB, the decoded picture
  // changed by 0.97 at it and 0.88 after, 0.00 between). With intra refresh a band of the picture is coded whole in each
  // frame and the whole picture is new every a.gop frames; a key frame at the start, every few seconds and when the goggles need one. An
  // encoder that does not open with it is opened again with the key frame every a.gop frames.
  std::vector<std::pair<std::string, bool>> tries;
  for (auto &name : order) { tries.push_back({name, true}); tries.push_back({name, false}); }
  for (auto &tr : tries) {
    const std::string &name = tr.first;
    const bool refresh = tr.second && a.gop > 0;
    const AVCodec *codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) continue;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->width = a.width; c->height = a.height;
    c->time_base = {1, a.fps}; c->framerate = {a.fps, 1};
    const bool qsv = name == "h264_qsv";
    c->pix_fmt = qsv ? AV_PIX_FMT_QSV : AV_PIX_FMT_D3D11; c->sw_pix_fmt = AV_PIX_FMT_NV12;
    // with intra refresh: NVENC takes gop_size as the refresh period and makes the GOP infinite (FFmpeg nvenc.c);
    // AMF and QSV get an IDR period no stream reaches (the refresh is set below)
    c->gop_size = refresh && name != "h264_nvenc" ? 1000000 : a.gop; c->max_b_frames = 0;
    c->bit_rate = c->rc_max_rate = c->rc_min_rate = b;
    c->rc_buffer_size = (int) (b / a.fps * a.vbv_frames);   // Apollo: bitrate / framerate (vbv_frames 1)
    c->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_CLOSED_GOP;
    c->slices = 1;
    c->color_range = AVCOL_RANGE_MPEG; c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709; c->color_trc = AVCOL_TRC_BT709;
    // Intel: a QSV device derived from the encoder's D3D11 device, QSV frames on it (Apollo video.cpp quicksync)
    AVBufferRef *base = hwdev, *qdev = nullptr;
    if (qsv) {
      if (av_hwdevice_ctx_create_derived(&qdev, AV_HWDEVICE_TYPE_QSV, hwdev, 0) < 0) {
        fprintf(stderr, "encoder h264_qsv did not open: no QSV device\n");
        avcodec_free_context(&c);
        continue;
      }
      base = qdev;
    }
    AVBufferRef *frames = av_hwframe_ctx_alloc(base);
    auto *fc = (AVHWFramesContext *) frames->data;
    fc->format = qsv ? AV_PIX_FMT_QSV : AV_PIX_FMT_D3D11; fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = a.width; fc->height = a.height;
    fc->initial_pool_size = 1;                                         // Apollo: a single texture
    if (!qsv)
      ((AVD3D11VAFramesContext *) fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;  // Apollo: the encoder needs it
    if (av_hwframe_ctx_init(frames) < 0) {
      fprintf(stderr, "encoder %s did not open: no frames\n", name.c_str());
      av_buffer_unref(&frames); av_buffer_unref(&qdev); avcodec_free_context(&c); continue;
    }
    av_buffer_unref(&qdev);
    c->hw_frames_ctx = frames;
    AVDictionary *o = nullptr;
    if (name == "h264_amf") {
      // Apollo video.cpp amdvce H.264 with config.cpp's defaults (usage ultralowlatency, rc vbr_latency,
      // enforce_hrd 0, preanalysis 0, vbaq 1, coder auto; amd_quality set to speed by the profiles)
      av_dict_set(&o, "filler_data", "0", 0);
      av_dict_set(&o, "forced_idr", "1", 0);
      av_dict_set(&o, "latency", "1", 0);
      av_dict_set(&o, "async_depth", "1", 0);
      av_dict_set(&o, "frame_skipping", "0", 0);
      av_dict_set(&o, "preencode", "0", 0);
      av_dict_set(&o, "quality", a.quality.c_str(), 0);   // Apollo option amd_quality (default balanced); the profiles: speed
      av_dict_set(&o, "rc", "vbr_latency", 0);
      av_dict_set(&o, "usage", "ultralowlatency", 0);
      av_dict_set(&o, "vbaq", "1", 0);
      av_dict_set(&o, "enforce_hrd", "0", 0);
      av_dict_set(&o, "header_spacing", std::to_string(a.gop).c_str(), 0);   // SPS/PPS every a.gop frames as well
      if (refresh)   // macroblocks coded whole per frame: the whole 1920x1088 picture (120 x 68) in a.gop frames
        av_dict_set(&o, "intra_refresh_mb", std::to_string((120 * 68 + a.gop - 1) / a.gop).c_str(), 0);
      av_dict_set(&o, "profile", "constrained_baseline", 0);   // what the simulator app sends the goggles
    } else if (qsv) {
      // Apollo video.cpp quicksync H.264 (config.cpp: qsv preset medium)
      av_dict_set(&o, "preset", "medium", 0);
      av_dict_set(&o, "forced_idr", "1", 0);
      av_dict_set(&o, "async_depth", "1", 0);
      av_dict_set(&o, "low_delay_brc", "1", 0);
      av_dict_set(&o, "low_power", "1", 0);
      av_dict_set(&o, "recovery_point_sei", "0", 0);
      av_dict_set(&o, "pic_timing_sei", "0", 0);
      av_dict_set(&o, "max_dec_frame_buffering", "1", 0);
      av_dict_set(&o, "profile", "baseline", 0);
      if (refresh) {
        av_dict_set(&o, "int_ref_type", "vertical", 0);
        av_dict_set(&o, "int_ref_cycle_size", std::to_string(a.gop).c_str(), 0);
      }
    } else if (name == "h264_nvenc") {
      // Apollo nvenc/nvenc_base.cpp with nvenc_config.h's defaults: P1, ultra low latency, CBR, zeroReorderDelay,
      // no lookahead, lowDelayKeyFrameScale 1, two pass at quarter resolution, no AQ, no filler
      av_dict_set(&o, "preset", "p1", 0);
      av_dict_set(&o, "tune", "ull", 0);
      av_dict_set(&o, "rc", "cbr", 0);
      av_dict_set(&o, "delay", "0", 0);
      av_dict_set(&o, "zerolatency", "1", 0);
      av_dict_set(&o, "rc-lookahead", "0", 0);
      av_dict_set(&o, "ldkfs", "1", 0);
      av_dict_set(&o, "multipass", "qres", 0);
      av_dict_set(&o, "forced-idr", "1", 0);
      av_dict_set(&o, "cbr_padding", "0", 0);
      av_dict_set(&o, "profile", "baseline", 0);
      if (refresh) av_dict_set(&o, "intra-refresh", "1", 0);
    }
    int r = avcodec_open2(c, codec, &o);
    av_dict_free(&o);
    if (r < 0) {
      char e[128];
      av_strerror(r, e, sizeof e);
      fprintf(stderr, "encoder %s%s did not open: %s\n", name.c_str(), refresh ? " with intra refresh" : "", e);
      avcodec_free_context(&c);
      continue;
    }
    enc = c; enc_name = name;
    fprintf(stderr, "refresh: %s\n", refresh ? "intra refresh" : "key frame every GOP");
    break;
  }
  if (!enc) die("no encoder opened");
  fprintf(stderr, "encoder %s %dx%d@%d %.0f Mbit/s, capture %ux%u\n", enc_name.c_str(), a.width, a.height, a.fps,
          a.bitrate, sw, sh);
  fflush(stderr);

  AVFrame *frame = av_frame_alloc();
  if (av_hwframe_get_buffer(enc->hw_frames_ctx, frame, 0) < 0) die("hw frame");
  // the D3D11 texture behind the encoder's frame (a QSV frame is mapped to D3D11 once, Apollo set_frame)
  AVFrame *d3d_frame = frame;
  if (frame->format != AV_PIX_FMT_D3D11) {
    d3d_frame = av_frame_alloc();
    d3d_frame->format = AV_PIX_FMT_D3D11;
    if (av_hwframe_map(d3d_frame, frame, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE) < 0) die("map QSV frame");
  }
  auto *enc_tex = (ID3D11Texture2D *) d3d_frame->data[0];
  const UINT enc_slice = (UINT) (intptr_t) d3d_frame->data[1];
  Com<ID3D11RenderTargetView> rtv_y, rtv_uv;
  {
    D3D11_RENDER_TARGET_VIEW_DESC r{};
    r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    r.Texture2DArray.FirstArraySlice = enc_slice;
    r.Texture2DArray.ArraySize = 1;
    r.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(edev->CreateRenderTargetView(enc_tex, &r, rtv_y.put()))) die("rtv y");
    r.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(edev->CreateRenderTargetView(enc_tex, &r, rtv_uv.put()))) die("rtv uv");
  }

  // Apollo: keep the aspect ratio (bars when the screen is not 16:9)
  float scalar = std::min((float) a.width / sw, (float) a.height / sh);
  float ow = sw * scalar, oh = sh * scalar, ox = (a.width - ow) / 2, oy = (a.height - oh) / 2;
  D3D11_VIEWPORT vp_y{ox, oy, ow, oh, 0, 1}, vp_uv{ox / 2, oy / 2, ow / 2, oh / 2, 0, 1};
  {
    const float yb[] = {0, 0, 0, 0}, uvb[] = {0.5f, 0.5f, 0.5f, 0.5f};
    ectx->ClearRenderTargetView(rtv_y, yb);
    ectx->ClearRenderTargetView(rtv_uv, uvb);
  }

  std::thread([] {   // key frame requests from Goggles Cast; its end closes our stdin
    int ch;
    while ((ch = getchar()) != EOF)
      if (ch == 'I') g_want_key = true;
    ExitProcess(0);   // Goggles Cast closed our stdin: it is gone, stop
  }).detach();

  // ---- the loop: capture thread (here) and encoder thread, like Apollo ----
  // The capture thread copies each new picture into a ring of textures (with the cursor drawn in) and hands it over;
  // the encoder thread converts and encodes. The hand-over holds one picture, the newest, as Apollo's images
  // (video.h img_event_t = safe::event_t, raise() replaces the value): a picture the encoder did not take in time is
  // replaced, never queued behind (a queue of 3 here until 0.8.20 let pictures wait up to 3 frames: latency jumps).
  int64_t freq; { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
  g_freq = freq;
  g_gop = a.gop;
  crc_tables();
  g_usb = new usbg::Link();
  g_usb->start();
  std::thread(sender_thread).detach();
  const int RING = 5, QMAX = 1;   // 1 waiting + 1 being converted, the rest free
  struct Slot {
    Com<ID3D11Texture2D> tex, etex;                 // the same picture on the capture / the encoder device
    Com<ID3D11RenderTargetView> rtv;                // capture side: the cursor is drawn into it
    Com<ID3D11ShaderResourceView> esrv;             // encoder side: the conversion reads it
    Com<IDXGIKeyedMutex> km, ekm;                   // key 1 = written, waiting for the encoder; key 0 = free
    std::atomic<int> key{0};
  } ring[RING];
  for (auto &sl : ring) {
    D3D11_TEXTURE2D_DESC t{};
    t.Width = sw; t.Height = sh; t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(dev->CreateTexture2D(&t, nullptr, sl.tex.put()))) die("ring texture");
    dev->CreateRenderTargetView(sl.tex, nullptr, sl.rtv.put());
    Com<IDXGIResource> r;
    HANDLE h = nullptr;
    if (FAILED(sl.tex->QueryInterface(__uuidof(IDXGIResource), (void **) r.put())) || FAILED(r->GetSharedHandle(&h)))
      die("ring share");
    if (FAILED(edev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void **) sl.etex.put()))) die("ring open");
    edev->CreateShaderResourceView(sl.etex, nullptr, sl.esrv.put());
    sl.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) sl.km.put());
    sl.etex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) sl.ekm.put());
  }
  struct Job { int slot; int64_t t_present, t_cap; bool ptr; };   // ptr: only the pointer changed
  std::mutex qm;
  std::condition_variable qcv;
  std::deque<Job> queue;
  std::atomic<int64_t> n_queue_drop{0}, n_waited{0}, max_queue{0};
  // the slot the encoder took (set under qm when it is taken): the capture never writes into it. Before 0.8.21 the
  // capture could read its key as 1, the encoder then release it with 0, and AcquireSync(1) wait forever
  std::atomic<int> enc_slot{-1};

  // pacing: Apollo's, not ours (display_base.cpp capture(), video.cpp encode_run())
  const double hz = ddesc.ModeDesc.RefreshRate.Denominator
      ? double(ddesc.ModeDesc.RefreshRate.Numerator) / ddesc.ModeDesc.RefreshRate.Denominator : 0.0;
  const int64_t frame_threshold = freq / a.fps;                 // encode_frame_threshold
  fprintf(stderr, "monitor %.2f Hz, stream %d fps, steady\n", hz, a.fps);

  // ---- encoder thread ----
  std::thread([&] {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    AVPacket *pkt = av_packet_alloc();
    int64_t pts = 0, last_stat = qpc(), frames_out = 0, bytes_out = 0, sum_cap_enc = 0, max_cap_enc = 0;
    int64_t sum_present_cap = 0, max_present_cap = 0;
    bool converted = false;
    // Steady output (GogglesCast, not Apollo). Apollo encodes each picture when it comes and Moonlight paces the frames
    // on the client (its Pacer: the newest frame at each display refresh). The goggles have no pacer and the maker's
    // app sends them a frame every 1/fps; ours came 5.7-30.5 ms apart with the mouse moving on the desktop (60 Hz,
    // measured 2026-10-29), and the user saw the frame rate jump and the picture break up exactly then. So the pacer
    // runs here: one frame per tick of a steady clock, the newest picture or the last one again; never two at once.
    // The ticks follow the game's pictures: a picture that just missed a tick moves the ticks 1/16 frame later, a
    // second in which every picture waited more than 3 ms moves them earlier, so a game at the stream's rate waits
    // ~1-3 ms for its tick.
    HANDLE etimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const int64_t F = frame_threshold;
    int64_t next_tick = 0, min_wait = INT64_MAX, last_game = 0, replaced_seen = 0;
    int waits_n = 0;
    for (;;) {
      Job j{-1, 0, 0, false};
      if (!next_tick) {   // the first picture starts the clock
        std::unique_lock<std::mutex> l(qm);
        qcv.wait_for(l, std::chrono::milliseconds(200), [&] { return !queue.empty(); });
        if (queue.empty()) continue;
        j = queue.front(); queue.pop_front(); enc_slot = j.slot;
        next_tick = qpc() + F;
      } else {
        const int64_t left = next_tick - qpc();
        if (left > 0) {
          LARGE_INTEGER due;
          due.QuadPart = -(left * 10000000LL / freq);
          if (etimer && SetWaitableTimerEx(etimer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(etimer, INFINITE);
          else Sleep((DWORD) (left * 1000 / freq));
        }
        const int64_t tick = next_tick;
        next_tick += F;
        {
          std::lock_guard<std::mutex> l(qm);
          if (!queue.empty()) { j = queue.front(); queue.pop_front(); enc_slot = j.slot; }
        }
        // two game pictures in one tick (one replaced) or none while the game runs: the pictures straddle the
        // tick, which moves 1/16 frame later
        const int64_t replaced = g_game_replaced.load();
        const bool straddle = replaced != replaced_seen || (j.slot < 0 && last_game && tick - last_game < 2 * F);
        replaced_seen = replaced;
        if (straddle) { next_tick += F / 16; ++g_late; }
        // the ticks follow the game's pictures; with no game (the desktop) the pointer's, which come on the capture's
        // own frame grid
        const bool steer = j.slot >= 0 && (!j.ptr || !last_game || tick - last_game > 2 * F);
        if (steer) {
          if (!j.ptr) last_game = tick;
          const int64_t waited = tick - j.t_cap;
          if (waited > F / 2) { if (!straddle) { next_tick += F / 16; ++g_late; } }   // came just after the last tick
          else min_wait = std::min(min_wait, waited);
          if (++waits_n >= a.fps) {
            if (min_wait != INT64_MAX && min_wait > freq * 3 / 1000) next_tick -= std::min(min_wait - freq / 1000, F / 16);
            min_wait = INT64_MAX;
            waits_n = 0;
          }
        }
      }
      if (j.slot < 0) {
        if (!converted) continue;
        ++g_repeats;
        j.t_present = j.t_cap = qpc();
      }
      if (j.slot >= 0) {
        converted = true;
        ring[j.slot].ekm->AcquireSync(1, INFINITE);
        // BGRA -> NV12 into the encoder's texture (Apollo convert())
        ID3D11ShaderResourceView *null_srv = nullptr;
        ID3D11RenderTargetView *null_rtv = nullptr;
        ectx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ectx->PSSetSamplers(0, 1, &sampler_e.p);
        ectx->PSSetConstantBuffers(0, 1, &cb_color.p);
        ectx->VSSetConstantBuffers(0, 1, &cb_subsample.p);
        ectx->VSSetConstantBuffers(1, 1, &cb_rotate_e.p);
        ectx->OMSetBlendState(blend_disable_e, nullptr, 0xFFFFFFFFu);
        ectx->PSSetShaderResources(0, 1, &ring[j.slot].esrv.p);
        ectx->OMSetRenderTargets(1, &rtv_y.p, nullptr);
        ectx->VSSetShader(vs_y, nullptr, 0);
        ectx->PSSetShader(ps_y, nullptr, 0);
        ectx->RSSetViewports(1, &vp_y);
        ectx->Draw(3, 0);
        ectx->OMSetRenderTargets(1, &rtv_uv.p, nullptr);
        ectx->VSSetShader(vs_uv, nullptr, 0);
        ectx->PSSetShader(ps_uv, nullptr, 0);
        ectx->RSSetViewports(1, &vp_uv);
        ectx->Draw(3, 0);
        ectx->PSSetShaderResources(0, 1, &null_srv);
        ectx->OMSetRenderTargets(1, &null_rtv, nullptr);
        ectx->Flush();   // the conversion goes to the GPU now, not when someone else happens to flush
        ring[j.slot].ekm->ReleaseSync(0);
        ring[j.slot].key = 0;
        enc_slot = -1;
      }
      // outside the context lock: avcodec_send_frame already runs AMF's submit + wait for the output (~5 ms), and the
      // capture thread must be able to take the next picture meanwhile (measured: a whole frame lost otherwise)
      frame->pts = pts++;
      // A key frame every 3 s as well, as DJI's Virtual Flight app sends these goggles (its I_FRAME_INTERVAL is 3).
      // 0.8.25 had one at the start and on request only, and the goggles stayed black for the user: the one key frame
      // sent when the stream starts is lost if the goggles have not switched to the simulator picture yet, and nothing
      // else ever gave their decoder a place to start (also after the goggles are taken off or leave their menu).
      // The intra refresh above still keeps the picture from pulsing between them; this one is 1 in 180 frames.
      static int since_key = 0;
      const bool periodic = ++since_key >= a.fps * 3;
      const bool want = g_want_key.exchange(false) || periodic;
      if (want) since_key = 0;
      frame->pict_type = want ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
      if (avcodec_send_frame(enc, frame) < 0) die("send_frame");
      int64_t t_sent = qpc();
      int got = 0, rr;
      while ((rr = avcodec_receive_packet(enc, pkt)) == 0) {
        ++got;
        if (g_trace)
          fprintf(stderr, "trace job pop_ms=%.2f recv_ms=%.2f pts=%lld\n", (t_sent - j.t_cap) * 1000.0 / freq,
                  (qpc() - t_sent) * 1000.0 / freq, (long long) pkt->pts);
        int64_t t_enc = qpc();
        {   // to the broadcast thread (Apollo: packets->raise(), a full queue is cleared)
          std::lock_guard<std::mutex> l(g_vq_m);
          if (g_vq.size() == 32) g_vq.clear();   // Apollo's safe::queue_t: cleared when full
          VPacket vp{std::vector<uint8_t>(pkt->data, pkt->data + pkt->size), (pkt->flags & AV_PKT_FLAG_KEY) != 0,
                     j.t_present, j.t_cap, t_enc};
          key_frame_headers(vp.au, vp.idr);
          g_vq.push_back(std::move(vp));
        }
        g_vq_cv.notify_one();
        ++frames_out; bytes_out += pkt->size;
        int64_t ce = t_enc - j.t_cap;
        sum_cap_enc += ce; max_cap_enc = std::max(max_cap_enc, ce);
        sum_present_cap += j.t_cap - j.t_present;
        max_present_cap = std::max(max_present_cap, j.t_cap - j.t_present);
        av_packet_unref(pkt);
      }
      int64_t now = qpc();
      if (next_tick && next_tick < now) { ++g_late_tick; next_tick = now; }   // an encode longer than a frame: no burst
      if (now - last_stat >= freq) {
        double dt = double(now - last_stat) / freq;
        fprintf(stderr, "stat fps=%.1f mbps=%.1f cap_enc_ms=%.2f cap_enc_max_ms=%.2f present_cap_ms=%.2f "
                "present_cap_max_ms=%.2f acc_lost=%lld late=%lld repeats=%lld ptr=%lld queue_drop=%lld "
                "late_tick=%lld max_queue=%lld presents=%lld usb_sent=%lld usb_oversize=%lld\n",
                frames_out / dt, bytes_out * 8 / dt / 1e6, frames_out ? sum_cap_enc * 1000.0 / freq / frames_out : 0.0,
                max_cap_enc * 1000.0 / freq, frames_out ? sum_present_cap * 1000.0 / freq / frames_out : 0.0,
                max_present_cap * 1000.0 / freq, (long long) g_acc_lost.exchange(0), (long long) g_late.exchange(0),
                (long long) g_repeats.exchange(0), (long long) g_ptr.exchange(0), (long long) n_queue_drop.exchange(0),
                (long long) g_late_tick.exchange(0), (long long) max_queue.exchange(0),
                (long long) (g_presents.exchange(0) / dt), (long long) g_usb->frames_sent.exchange(0),
                (long long) g_usb->frames_oversize.exchange(0));
        fflush(stderr);
        last_stat = now; frames_out = bytes_out = sum_cap_enc = max_cap_enc = sum_present_cap = max_present_cap = 0;
      }
    }
  }).detach();

  // ---- capture thread: Apollo display_base.cpp capture() + display_vram.cpp snapshot() ----
  CursorTex cur_alpha, cur_xor;
  bool cursor_visible = false;
  LONG cur_x = 0, cur_y = 0;
  std::vector<uint8_t> shape, prev_shape;
  DXGI_OUTDUPL_POINTER_SHAPE_INFO prev_si{};
  std::vector<RECT> dirty;
  bool have_image = false;
  int next_slot = 0;
  int64_t snap_stamp = 0;
  bool snap_ptr = false;   // the last picture is a pointer change only
  const int64_t ptr_wait = freq * 3 / (2 * a.fps);
  int64_t last_frame_at = 0;   // when the last new picture from Windows (not only the pointer) was taken
  bool ptr_pending = false;     // a pointer change waits for the next picture (at most ptr_wait after last_frame_at)
  // snapshot(): 0 = a new picture or a pointer update (picture in `cap`, cursor state updated), 1 = nothing new
  auto snapshot = [&](UINT timeout_ms) -> int {
    DXGI_OUTDUPL_FRAME_INFO fi{};
    Com<IDXGIResource> res;
    HRESULT hr = dup->AcquireNextFrame(timeout_ms, &fi, res.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 1;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_ACCESS_DENIED || hr == (HRESULT) WAIT_ABANDONED) {
      // Apollo display_base.cpp: capture_e::reinit, and video.cpp then makes the display and the encoders anew (after
      // a mode change the new size needs new textures). Here: exit code 3, the program starts the capture again.
      fprintf(stderr, "duplication lost (0x%08lx): reinit\n", (unsigned long) hr);
      fflush(stderr);
      ExitProcess(3);
    }
    if (hr == DXGI_ERROR_INVALID_CALL) {
      fprintf(stderr, "duplication lost (0x%08lx), reopening\n", (unsigned long) hr);
      Sleep(100);
      if (FAILED(open_dup())) Sleep(500);
      return 1;
    }
    if (FAILED(hr)) die("AcquireNextFrame", hr);
    const bool frame_update = fi.LastPresentTime.QuadPart != 0;
    // the one difference from Apollo: a pointer report is a new picture only when the cursor is drawn and really
    // moved or changed shape. Windows reports the pointer ~5-12 times a second also when nothing moved (measured);
    // as pictures they took the game's slots on the frame grid and the game frame after each was dropped as too fast
    // (48 of 60 a second in full_session_test)
    bool mouse_update = false;
    if (a.cursor && fi.PointerShapeBufferSize) {
      shape.resize(fi.PointerShapeBufferSize);
      UINT need = 0;
      DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
      if (SUCCEEDED(dup->GetFramePointerShape((UINT) shape.size(), shape.data(), &need, &si)) &&
          !(shape == prev_shape && si.Type == prev_si.Type && si.Width == prev_si.Width && si.Height == prev_si.Height &&
            si.HotSpot.x == prev_si.HotSpot.x && si.HotSpot.y == prev_si.HotSpot.y)) {
        if (g_trace) fprintf(stderr, "trace ptr shape type=%u %ux%u hot=%d,%d bytes=%u\n", si.Type, si.Width, si.Height,
                             (int) si.HotSpot.x, (int) si.HotSpot.y, need);
        prev_shape = shape; prev_si = si;
        UINT h = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? si.Height / 2 : si.Height;
        make_cursor_tex(dev, cur_alpha, cursor_alpha_image(shape, si), si.Width, h);
        make_cursor_tex(dev, cur_xor, cursor_xor_image(shape, si), si.Width, h);
        mouse_update = true;
      }
    }
    if (fi.LastMouseUpdateTime.QuadPart) {
      const bool vis = fi.PointerPosition.Visible;
      const LONG x = fi.PointerPosition.Position.x, y = fi.PointerPosition.Position.y;
      if (a.cursor && (vis != cursor_visible || (vis && (x != cur_x || y != cur_y)))) {
        mouse_update = true;
        if (g_trace) fprintf(stderr, "trace ptr pos vis=%d->%d %ld,%ld->%ld,%ld\n", (int) cursor_visible, (int) vis, cur_x, cur_y, x, y);
      }
      cursor_visible = vis; cur_x = x; cur_y = y;
    }
    if (!mouse_update && !frame_update) { dup->ReleaseFrame(); return 1; }
    // While Windows is sending new pictures (a game), a pointer change goes into the next one, which comes within a
    // frame, and makes no picture of its own: as its own picture it moved the capture's frame grid off the game's,
    // and game frames were lost in Windows' queue (measured 2026-10-27 and again 2026-10-29 with the steady output:
    // an animated cursor, 16-18 game frames a second lost). On a still screen (the desktop) every pointer change is a
    // picture at once, as in Apollo; when Windows' next picture does not come within 1.5 frames, the pointer change
    // gets its own (the capture loop, ptr_pending).
    if (!frame_update && have_image && qpc() - last_frame_at < ptr_wait) {
      ptr_pending = true;
      dup->ReleaseFrame();
      return 1;
    }
    if (!frame_update) ++g_ptr;
    // a new picture: when Windows showed it; a pointer change: when the pointer changed (Apollo takes the later of
    // the two for both, which gives a game frame the pointer's time and moves it closer to the next game frame)
    const int64_t stamp = frame_update ? fi.LastPresentTime.QuadPart : fi.LastMouseUpdateTime.QuadPart;
    snap_ptr = !frame_update;
    if (frame_update) {
      if (fi.AccumulatedFrames > 1 && have_image) g_acc_lost += fi.AccumulatedFrames - 1;
      // statistics only (the window's "picture N"): a game changes a large part of the screen; a clock or a
      // tooltip on the desktop also makes Windows report a new picture (5-20 a second, measured)
      if (fi.TotalMetadataBufferSize) {
        dirty.resize(fi.TotalMetadataBufferSize / sizeof(RECT) + 1);
        UINT got = 0;
        if (SUCCEEDED(dup->GetFrameDirtyRects((UINT) (dirty.size() * sizeof(RECT)), dirty.data(), &got))) {
          int64_t area = 0;
          for (UINT i = 0; i < got / sizeof(RECT); ++i)
            area += (int64_t) (dirty[i].right - dirty[i].left) * (dirty[i].bottom - dirty[i].top);
          if (area * 4 >= (int64_t) sw * sh) g_presents += std::max<UINT>(1, fi.AccumulatedFrames);
        }
      }
      Com<ID3D11Texture2D> src;
      res->QueryInterface(__uuidof(ID3D11Texture2D), (void **) src.put());
      ctx->CopyResource(cap, src);
      have_image = true;
      last_frame_at = qpc();
    }
    dup->ReleaseFrame();
    if (!have_image) return 1;
    snap_stamp = stamp;
    return 0;
  };
  // Apollo's timer->sleep_for: a high-resolution waitable timer
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  auto sleep_ticks = [&](int64_t ticks) {
    LARGE_INTEGER due;
    due.QuadPart = -(ticks * 10000000LL / freq);
    if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD) (ticks * 1000 / freq));
  };
  // Apollo: keep the display awake during capture (asleep, capture stops; a screen saver ends Desktop Duplication)
  SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED);
  // Apollo adjust_client_frame_rate(): a display at e.g. 59.94 Hz gets a capture grid at exactly its rate, or the grid
  // drifts against the display's refreshes and pictures miss their window (seen: 47 of 60 a second)
  uint64_t rate_num = (uint64_t) a.fps, rate_den = 1;
  {
    const DXGI_RATIONAL r = ddesc.ModeDesc.RefreshRate;
    if (r.Denominator > 1) {
      const int rounded = (int) std::lround((double) r.Numerator / r.Denominator);
      uint64_t cn = r.Numerator, cd = r.Denominator;
      if (rounded > 0 && a.fps % rounded == 0) cn *= a.fps / rounded;
      else if (rounded > 0 && rounded % a.fps == 0) cd *= rounded / a.fps;
      const double cr = (double) cn / cd;
      if (a.fps > cr && cr / a.fps > 0.99) {
        rate_num = cn; rate_den = cd;
        fprintf(stderr, "capture rate adjusted to %.3f fps to match the display\n", cr);
      }
    }
  }
  const int64_t ms2 = freq / 500;
  bool group = false;            // frame_pacing_group_start is set
  int64_t group_start = 0;
  int64_t group_frames = 0;
  for (;;) {
    int status = 0;
    if (group) {   // continue the frame pacing group: the next picture at the client's frame interval
      const int64_t target = group_start + (int64_t) ((uint64_t) group_frames * rate_den * (uint64_t) freq / rate_num);
      const int64_t period = target - qpc();
      if (period <= 0) {   // missed the next frame time
        group = false; group_frames = 0; status = 1;
      } else {
        bool elastic = false;
        if (period >= ms2) { elastic = true; sleep_ticks(period - ms2); }
        status = snapshot(elastic ? 2 : (UINT) (period * 1000 / freq));
        if (status == 0) ++group_frames;
        else { group = false; group_frames = 0; }
      }
    }
    if (status == 1 || (status == 0 && !group)) {   // start a new frame pacing group
      UINT wait_ms = 200;
      if (ptr_pending) {   // not past the pointer change's deadline
        const int64_t left = last_frame_at + ptr_wait - qpc();
        wait_ms = left > 0 ? (UINT) (left * 1000 / freq) + 1 : 0;
      }
      status = snapshot(wait_ms);
      if (status == 0) { group_start = snap_stamp; group = true; group_frames = 1; }
      else if (have_image && ptr_pending && qpc() - last_frame_at >= ptr_wait) {
        snap_stamp = qpc();
        snap_ptr = true;
      }
      else if (ptr_pending) continue;
      else { Sleep(10); continue; }   // Apollo: let the encoder thread have the device while nothing updates
    }
    const int64_t t_present = snap_stamp, t_cap = qpc();
    // into the next ring slot, with the cursor (Apollo blend_cursor)
    int slot;
    {
      std::lock_guard<std::mutex> l(qm);
      slot = next_slot;
      if (slot == enc_slot.load()) slot = (slot + 1) % RING;   // the encoder is converting that one
      next_slot = (slot + 1) % RING;
      for (auto it = queue.begin(); it != queue.end(); ++it)   // still waiting there: this picture replaces it
        if (it->slot == slot) { queue.erase(it); ++n_queue_drop; break; }
    }
    {
      ring[slot].km->AcquireSync(ring[slot].key, INFINITE);   // 1 when its frame was dropped from the queue unread
      ctx->CopyResource(ring[slot].tex, cap);
      const bool draw_cursor = a.cursor && cursor_visible && (cur_alpha.srv || cur_xor.srv);
      if (draw_cursor) {
        ID3D11ShaderResourceView *null_srv = nullptr;
        ctx->VSSetShader(vs_cursor, nullptr, 0);
        ctx->PSSetShader(ps_cursor, nullptr, 0);
        ctx->VSSetConstantBuffers(2, 1, &cb_rotate.p);
        ctx->PSSetSamplers(0, 1, &sampler.p);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->OMSetRenderTargets(1, &ring[slot].rtv.p, nullptr);
        if (draw_cursor && cur_alpha.srv) {
          D3D11_VIEWPORT v{(float) cur_x, (float) cur_y, (float) cur_alpha.w, (float) cur_alpha.h, 0, 1};
          ctx->OMSetBlendState(blend_alpha, nullptr, 0xFFFFFFFFu);
          ctx->PSSetShaderResources(0, 1, &cur_alpha.srv.p);
          ctx->RSSetViewports(1, &v);
          ctx->Draw(3, 0);
        }
        if (draw_cursor && cur_xor.srv) {
          D3D11_VIEWPORT v{(float) cur_x, (float) cur_y, (float) cur_xor.w, (float) cur_xor.h, 0, 1};
          ctx->OMSetBlendState(blend_invert, nullptr, 0x00FFFFFFu);
          ctx->PSSetShaderResources(0, 1, &cur_xor.srv.p);
          ctx->RSSetViewports(1, &v);
          ctx->Draw(3, 0);
        }
        ctx->OMSetBlendState(blend_disable, nullptr, 0xFFFFFFFFu);
        ctx->PSSetShaderResources(0, 1, &null_srv);
        ID3D11RenderTargetView *null_rtv = nullptr;
        ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
      }
      ring[slot].km->ReleaseSync(1);
      ring[slot].key = 1;
    }
    {
      std::lock_guard<std::mutex> l(qm);
      bool ptr = snap_ptr;
      if ((int) queue.size() >= QMAX) {   // not taken yet: the newer picture replaces it (raise()), counted
        ptr = ptr && queue.front().ptr;   // a pointer picture over a game picture carries the game picture
        if (!queue.front().ptr) ++g_game_replaced;
        queue.pop_front();
        ++n_queue_drop;
      }
      queue.push_back({slot, t_present, t_cap, ptr});
      ptr_pending = false;   // the pointer as it is now is in this picture
      int64_t qs = (int64_t) queue.size();
      if (qs > max_queue) max_queue = qs;
    }
    qcv.notify_one();
  }
}
