// present_test: a borderless window on the primary monitor that presents every vsync like a game (D3D11 flip model,
// colour changes every frame), and prints the presents per second on stdout. For checking that a monitor
// (e.g. the virtual one) really runs at its rate: gcast_capture must then see the same number of frames.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) PostQuitMessage(0);
  return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t **argv) {
  double seconds = argc > 1 ? _wtof(argv[1]) : 8;
  double rate = argc > 2 ? _wtof(argv[2]) : 0;   // like a game that only manages this many frames a second (0 = every vsync)
  WNDCLASSW wc{};
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"present_test";
  RegisterClassW(&wc);
  int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
  HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"present_test", WS_POPUP | WS_VISIBLE, 0, 0, w, h,
                              nullptr, nullptr, wc.hInstance, nullptr);
  ID3D11Device *dev; ID3D11DeviceContext *ctx;
  D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
  IDXGIDevice *dx; dev->QueryInterface(__uuidof(IDXGIDevice), (void **) &dx);
  IDXGIAdapter *ad; dx->GetAdapter(&ad);
  IDXGIFactory2 *f; ad->GetParent(__uuidof(IDXGIFactory2), (void **) &f);
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1 *sc;
  if (FAILED(f->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc))) return 1;
  ID3D11Texture2D *bb; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **) &bb);
  ID3D11RenderTargetView *rtv; dev->CreateRenderTargetView(bb, nullptr, &rtv);
  LARGE_INTEGER fq, t0, t; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
  LARGE_INTEGER last = t0;
  int n = 0, total = 0, late = 0;
  LARGE_INTEGER prev = t0;
  double hz_guess = 0;
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) return 0; DispatchMessageW(&msg); }
    float c = (total % 2) ? 1.0f : 0.0f, col[4] = {c, 0.3f, 1.0f - c, 1};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, col);
    if (rate > 0) {   // wait for this frame's turn, then show it at the next vsync
      LARGE_INTEGER now;
      const LONGLONG due = t0.QuadPart + (LONGLONG) (total * fq.QuadPart / rate);
      for (QueryPerformanceCounter(&now); now.QuadPart < due; QueryPerformanceCounter(&now))
        Sleep(0);   // not Sleep(1): without timeBeginPeriod it sleeps ~15 ms and two frames land in one vsync
      sc->Present(0, 0);
    } else {
      sc->Present(1, 0);
    }
    ++n; ++total;
    QueryPerformanceCounter(&t);
    // the window's own hiccups: a present that came more than 1.5 intervals after the previous one
    double gap = double(t.QuadPart - prev.QuadPart) / fq.QuadPart;
    prev = t;
    if (total > 30 && hz_guess > 0 && gap > 1.5 / hz_guess) ++late;
    if (t.QuadPart - last.QuadPart >= fq.QuadPart) {
      hz_guess = n * (double) fq.QuadPart / (t.QuadPart - last.QuadPart);
      printf("presents %.1f/s late %d\n", hz_guess, late);
      late = 0;
      fflush(stdout);
      n = 0; last = t;
    }
    if (t.QuadPart - t0.QuadPart > seconds * fq.QuadPart) return 0;
  }
}
