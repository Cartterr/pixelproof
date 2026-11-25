#include "preview_win.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gst/video/videooverlay.h>

#include <cstdlib>
#include <future>
#include <thread>

#pragma comment(lib, "user32.lib")

namespace sb {
namespace {

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  if (m == WM_SETCURSOR) {
    SetCursor(nullptr);  // hide the cursor over the preview
    return TRUE;
  }
  return DefWindowProcA(h, m, w, l);
}

BOOL CALLBACK enum_mon(HMONITOR m, HDC, LPRECT, LPARAM p) {
  MONITORINFOEXA mi{};
  mi.cbSize = sizeof mi;
  GetMonitorInfoA(m, &mi);
  auto* v = reinterpret_cast<std::vector<MonitorInfo>*>(p);
  v->push_back({mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top, reinterpret_cast<unsigned long long>(m),
                (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
  return TRUE;
}

}  // namespace

std::vector<MonitorInfo> list_monitors() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::vector<MonitorInfo> v;
  EnumDisplayMonitors(nullptr, nullptr, enum_mon, reinterpret_cast<LPARAM>(&v));
  return v;
}

PreviewWindows::~PreviewWindows() {
  for (void* h : hwnds_) PostMessageA(static_cast<HWND>(h), WM_CLOSE, 0, 0);
  for (void* t : threads_) {
    auto* th = static_cast<std::thread*>(t);
    if (th->joinable()) th->join();
    delete th;
  }
}

bool PreviewWindows::create(const std::string& spec) {
  auto mons = list_monitors();
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t comma = spec.find(',', pos);
    std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = comma == std::string::npos ? spec.size() + 1 : comma + 1;
    if (item.empty()) continue;
    const MonitorInfo* m = nullptr;
    char* end = nullptr;
    long idx = std::strtol(item.c_str(), &end, 10);
    if (end && *end == 0 && idx >= 0 && idx < static_cast<long>(mons.size())) m = &mons[idx];
    for (auto& x : mons)
      if (x.device == item) m = &x;
    if (!m) return false;
    MonitorInfo mi = *m;
    std::promise<HWND> pr;
    auto fut = pr.get_future();
    auto* th = new std::thread([mi, &pr] {
      WNDCLASSA wc{};
      wc.lpfnWndProc = wndproc;
      wc.hInstance = GetModuleHandleA(nullptr);
      wc.lpszClassName = "sb_preview";
      wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
      RegisterClassA(&wc);  // fails harmlessly if already registered
      HWND h = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "sb_preview", "sb_sender preview",
                               WS_POPUP | WS_VISIBLE, mi.x, mi.y, mi.w, mi.h, nullptr, nullptr, wc.hInstance, nullptr);
      pr.set_value(h);
      MSG msg;
      while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
      }
    });
    HWND h = fut.get();
    if (!h) return false;
    hwnds_.push_back(h);
    threads_.push_back(th);
    desc_ += (desc_.empty() ? "" : ",") + mi.device + "(" + std::to_string(mi.w) + "x" + std::to_string(mi.h) + ")";
  }
  return !hwnds_.empty();
}

void PreviewWindows::attach(GstElement* pipeline, const std::string& prefix) {
  for (size_t i = 0; i < hwnds_.size(); ++i) {
    GstElement* s = gst_bin_get_by_name(GST_BIN(pipeline), (prefix + std::to_string(i)).c_str());
    if (!s) continue;
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(s), reinterpret_cast<guintptr>(hwnds_[i]));
    gst_object_unref(s);
  }
}

}  // namespace sb

#else  // !_WIN32

namespace sb {
std::vector<MonitorInfo> list_monitors() { return {}; }
PreviewWindows::~PreviewWindows() = default;
bool PreviewWindows::create(const std::string&) { return false; }
void PreviewWindows::attach(GstElement*, const std::string&) {}
}  // namespace sb

#endif
