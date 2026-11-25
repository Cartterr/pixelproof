// Borderless, top-most preview windows that cover whole monitors (Windows only). The sender
// renders the stamped frames into them through GstVideoOverlay, so the same pattern can be shown
// full-screen on the laptop panel (for the camera) and on a virtual 4K monitor (for Sunshine).
#pragma once
#include <gst/gst.h>

#include <string>
#include <vector>

namespace sb {

struct MonitorInfo {
  std::string device;  // \\.\DISPLAYn
  int x, y, w, h;
  unsigned long long handle;  // HMONITOR
  bool primary;
};
std::vector<MonitorInfo> list_monitors();

class PreviewWindows {
 public:
  ~PreviewWindows();
  // spec: comma-separated monitor indices (list_monitors order) or device names.
  bool create(const std::string& spec);
  size_t count() const { return hwnds_.size(); }
  // Hands window i to the sink element named name (must implement GstVideoOverlay).
  void attach(GstElement* pipeline, const std::string& sink_prefix);
  std::string describe() const { return desc_; }

 private:
  std::vector<void*> hwnds_;
  std::vector<void*> threads_;
  std::string desc_;
};

}  // namespace sb
