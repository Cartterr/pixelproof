#include "sources.h"

#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

#include <cstdio>

#include "../common/pattern.h"
#include "../common/util.h"
#include "preview_win.h"

namespace sb {
namespace {

class PatternSource : public Source {
 public:
  explicit PatternSource(const SourceConfig& c) : c_(c), gen_(c.width, c.height, c.fps, c.scene.c_str()) {
    gst_video_info_set_format(&info_, GST_VIDEO_FORMAT_RGB, c.width, c.height);
  }
  ~PatternSource() override { stop(); }
  std::string describe() const override {
    return "pattern(" + c_.scene + ") " + std::to_string(c_.width) + "x" + std::to_string(c_.height) + "@" + std::to_string(c_.fps);
  }
  std::string fragment() const override {
    char b[512];
    std::snprintf(b, sizeof b,
                  "appsrc name=patsrc is-live=true format=time do-timestamp=true block=false "
                  "max-buffers=2 leaky-type=downstream "
                  "caps=video/x-raw,format=RGB,width=%d,height=%d,framerate=%d/1,"
                  "colorimetry=sRGB,pixel-aspect-ratio=1/1",
                  c_.width, c_.height, c_.fps);
    return b;
  }
  bool exact_rate() const override { return true; }
  void attach(GstElement* pipeline) override { appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline), "patsrc"); }
  void start() override {
    run_ = true;
    th_ = std::thread([this] { loop(); });
  }
  void stop() override {
    run_ = false;
    if (th_.joinable()) th_.join();
    if (appsrc_) {
      gst_object_unref(appsrc_);
      appsrc_ = nullptr;
    }
  }
  uint64_t produced() const override { return n_; }

 private:
  void loop() {
    const int64_t period = 1'000'000'000LL / c_.fps;
    int64_t t0 = now_ns() + 50'000'000;
    for (uint64_t i = 0; run_; ++i) {
      int64_t due = t0 + static_cast<int64_t>(i) * period;
      precise_sleep_until(due);
      // If we fell more than 2 frames behind (e.g. machine stalled), skip ahead rather than burst.
      int64_t late = now_ns() - due;
      if (late > 2 * period) {
        uint64_t skip = static_cast<uint64_t>(late / period);
        i += skip;
        continue;
      }
      GstBuffer* buf = gst_buffer_new_allocate(nullptr, info_.size, nullptr);
      GstVideoFrame f;
      if (!gst_video_frame_map(&f, &info_, buf, GST_MAP_WRITE)) {
        gst_buffer_unref(buf);
        continue;
      }
      gen_.render(static_cast<uint32_t>(i), static_cast<uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&f, 0)),
                  GST_VIDEO_FRAME_PLANE_STRIDE(&f, 0));
      gst_video_frame_unmap(&f);
      GST_BUFFER_OFFSET(buf) = i;  // motion index travels with the buffer
      gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
      n_ = i + 1;
    }
  }

  SourceConfig c_;
  PatternGen gen_;
  GstVideoInfo info_;
  GstElement* appsrc_ = nullptr;
  std::thread th_;
  std::atomic<bool> run_{false};
  std::atomic<uint64_t> n_{0};
};

class DesktopSource : public Source {
 public:
  explicit DesktopSource(const SourceConfig& c) : c_(c) {}
  std::string describe() const override {
    return "desktop monitor " + c_.monitor + " via " + c_.capture_api;
  }
  std::string fragment() const override {
#ifdef _WIN32
    // Capture on the GPU, scale + convert on the GPU (letterboxed, aspect kept), then download.
    // Monitor by device name (e.g. DISPLAY1 or \\.\DISPLAY1, resolved to an HMONITOR) or by DXGI index.
    std::string sel = "monitor-index=" + c_.monitor;
    if (c_.monitor.find("DISPLAY") != std::string::npos) {
      std::string want = c_.monitor.substr(c_.monitor.find("DISPLAY"));
      for (auto& m : list_monitors())
        if (m.device.size() >= want.size() && m.device.compare(m.device.size() - want.size(), want.size(), want) == 0)
          sel = "monitor-handle=" + std::to_string(m.handle);
    }
    char b[768];
    std::snprintf(b, sizeof b,
                  "d3d11screencapturesrc name=cap %s show-cursor=%s capture-api=%s ! "
                  "video/x-raw(memory:D3D11Memory),framerate=%d/1 ! "
                  "d3d11convert ! video/x-raw(memory:D3D11Memory),format=RGBA,width=%d,height=%d,"
                  "pixel-aspect-ratio=1/1 ! d3d11download",
                  sel.c_str(), c_.show_cursor ? "true" : "false", c_.capture_api.c_str(), c_.fps,
                  c_.width, c_.height);
    return b;
#else
    return "ximagesrc use-damage=false";
#endif
  }
  bool exact_rate() const override { return true; }

 private:
  SourceConfig c_;
};

class CaptureSource : public Source {
 public:
  explicit CaptureSource(const SourceConfig& c) : c_(c) {}
  std::string describe() const override { return "capture device " + c_.device; }
  std::string fragment() const override {
#ifdef _WIN32
    return "mfvideosrc device-index=" + (c_.device.empty() ? std::string("0") : c_.device);
#else
    return "v4l2src device=" + (c_.device.empty() ? std::string("/dev/video0") : c_.device) +
           " ! decodebin";
#endif
  }

 private:
  SourceConfig c_;
};

class CustomSource : public Source {
 public:
  explicit CustomSource(const SourceConfig& c) : c_(c) {}
  std::string describe() const override { return "custom: " + c_.custom; }
  std::string fragment() const override { return c_.custom; }

 private:
  SourceConfig c_;
};

}  // namespace

std::unique_ptr<Source> make_source(const SourceConfig& cfg) {
  if (cfg.kind == "pattern") return std::make_unique<PatternSource>(cfg);
  if (cfg.kind == "desktop") return std::make_unique<DesktopSource>(cfg);
  if (cfg.kind == "capture") return std::make_unique<CaptureSource>(cfg);
  if (cfg.kind.rfind("gst:", 0) == 0) {
    SourceConfig c = cfg;
    c.custom = cfg.kind.substr(4);
    return std::make_unique<CustomSource>(c);
  }
  return nullptr;
}

}  // namespace sb
