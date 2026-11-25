// Input boundary for the sender. A Source contributes a GStreamer launch fragment that ends in
// raw video (any format/size); the sender appends the common convert/scale/stamp/encode/payload
// tail. Adding a new input (e.g. a USB capture card) means adding a Source here, nothing else.
//
//   pattern            deterministic moving test pattern (appsrc, generated in this process)
//   desktop            Windows desktop via Desktop Duplication (d3d11screencapturesrc), GPU scale
//   capture            USB/UVC capture device (mfvideosrc on Windows, v4l2src on Linux)
//   gst:<fragment>     any GStreamer fragment producing raw video
#pragma once
#include <gst/gst.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace sb {

struct SourceConfig {
  std::string kind = "pattern";
  int width = 960, height = 540, fps = 60;
  std::string monitor = "0";     // desktop: DXGI monitor index, or a device name like \.\DISPLAY1
  std::string capture_api = "dxgi";  // desktop: dxgi (Desktop Duplication) or wgc
  bool show_cursor = true;
  std::string device;            // capture: device index (Windows) or /dev/videoN (Linux)
  std::string custom;            // gst:<fragment>
  std::string scene = "test";    // pattern: test | text | static
};

class Source {
 public:
  virtual ~Source() = default;
  virtual std::string describe() const = 0;
  // Fragment that produces raw video. Must not end with '!'.
  virtual std::string fragment() const = 0;
  // True when the source already delivers exactly WxH@fps (skip videorate).
  virtual bool exact_rate() const { return false; }
  virtual void attach(GstElement* /*pipeline*/) {}
  virtual void start() {}
  virtual void stop() {}
  // Motion index of the frame the source produced (pattern only), for logging.
  virtual uint64_t produced() const { return 0; }
};

std::unique_ptr<Source> make_source(const SourceConfig& cfg);

}  // namespace sb
