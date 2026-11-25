// sb_sender: capture/generate video, stamp a frame ID, hash the canonical RGB bytes right before
// packetisation, and send it over RTP/UDP (raw RFC 4175 or FFV1 in rtpgstpay).
//
// Example:
//   sb_sender --source pattern --width 960 --height 540 --fps 60 --codec raw
//             --dest 10.77.0.2 --bind 10.77.0.1 --warmup 5 --duration 30 --csv sender.csv
#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/canon.h"
#include "../common/proto.h"
#include "../common/stamp.h"
#include "../common/util.h"
#include "preview_win.h"
#include "sources.h"

using namespace sb;

namespace {

// ------------------------------------------------------------------ clock offset estimation
// NTP-style samples; offset(t) = receiver_clock - sender_clock, fitted over the best (minimum
// RTT) sample of each 1 s bucket in the last 20 s, so slow drift between the two crystals is
// tracked. Only "good" samples are used: RTT <= 3x the best RTT ever seen (+0.2 ms). Pings that
// queued behind video bursts are ignored; if none are good for a while, the last fit is kept and
// its uncertainty grows by a 20 ppm drift allowance per second elapsed since the last good sample.
// Uncertainty = half the worst RTT among the samples used + fit residual (+ that allowance).
class ClockSync {
 public:
  void add(int64_t t1, int64_t t2, int64_t t3) {
    std::lock_guard<std::mutex> lk(mu_);
    int64_t rtt = t3 - t1;
    if (rtt <= 0) return;
    best_rtt_ = std::min(best_rtt_, rtt);
    if (rtt > 3 * best_rtt_ + 200'000) {
      ++rejected_;
      return;
    }
    samples_.push_back({(t1 + t3) / 2, t2 - (t1 + t3) / 2, rtt});
    while (!samples_.empty() && samples_.back().tmid - samples_.front().tmid > 20'000'000'000LL)
      samples_.pop_front();
    t_last_good_ = samples_.back().tmid;
    refit();
  }
  bool map(int64_t ts, int64_t* tr, uint32_t* unc) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!valid_) return false;
    double off = a_ + b_ * static_cast<double>(ts - tref_);
    *tr = ts + static_cast<int64_t>(std::llround(off));
    *unc = unc_ + static_cast<uint32_t>(20e-6 * static_cast<double>(std::max<int64_t>(0, ts - t_last_good_)));
    return true;
  }
  double best_rtt_ms() const { std::lock_guard<std::mutex> lk(mu_); return best_rtt_ / 1e6; }
  double unc_ms() const {
    int64_t tr;
    uint32_t u;
    return map(now_ns(), &tr, &u) ? u / 1e6 : -1;
  }
  double drift_ppm() const { std::lock_guard<std::mutex> lk(mu_); return b_ * 1e6; }
  uint64_t rejected() const { std::lock_guard<std::mutex> lk(mu_); return rejected_; }

 private:
  struct S { int64_t tmid, off, rtt; };
  void refit() {
    std::map<int64_t, S> best;  // per 1 s bucket
    for (auto& s : samples_) {
      int64_t k = s.tmid / 1'000'000'000LL;
      auto it = best.find(k);
      if (it == best.end() || s.rtt < it->second.rtt) best[k] = s;
    }
    std::vector<S> v;
    for (auto& [k, s] : best) v.push_back(s);
    if (v.empty()) return;
    tref_ = v.back().tmid;
    int64_t worst_rtt = 0;
    for (auto& s : v) worst_rtt = std::max(worst_rtt, s.rtt);
    if (v.size() < 3) {
      auto m = *std::min_element(v.begin(), v.end(), [](const S& x, const S& y) { return x.rtt < y.rtt; });
      a_ = static_cast<double>(m.off) + 0.0;
      b_ = 0;
      tref_ = m.tmid;
      unc_ = static_cast<uint32_t>(m.rtt / 2 + 20'000);
    } else {
      double sx = 0, sy = 0, sxx = 0, sxy = 0;
      int n = static_cast<int>(v.size());
      for (auto& s : v) {
        double x = static_cast<double>(s.tmid - tref_), y = static_cast<double>(s.off);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
      }
      double den = n * sxx - sx * sx;
      b_ = den != 0 ? (n * sxy - sx * sy) / den : 0;
      a_ = (sy - b_ * sx) / n;
      double res = 0;
      for (auto& s : v) {
        double x = static_cast<double>(s.tmid - tref_);
        res = std::max(res, std::fabs(static_cast<double>(s.off) - (a_ + b_ * x)));
      }
      unc_ = static_cast<uint32_t>(worst_rtt / 2 + res + 20'000);
    }
    valid_ = true;
  }

  mutable std::mutex mu_;
  std::deque<S> samples_;
  bool valid_ = false;
  double a_ = 0, b_ = 0;
  int64_t tref_ = 0;
  uint32_t unc_ = 0;
  int64_t best_rtt_ = INT64_MAX;
  int64_t t_last_good_ = 0;
  uint64_t rejected_ = 0;
};

struct FrameInfo {
  uint32_t id = 0;
  uint32_t flags = 0;
  int64_t t_src = 0, t_qout = 0, t_pay = 0;
  uint64_t hash = 0;
  uint32_t payload_bytes = 0;
  bool sent = false;
};

struct Sender {
  // config
  std::string codec = "raw", dest, bind, label, csv, perframe_path, timeseries_path;
  int width = 960, height = 540, fps = 60, port = 5004, ctrl_port = 5006, mtu = 1400;
  double warmup_s = 5, duration_s = 30;
  bool stamp = true;

  // state
  GstElement* pipeline = nullptr;
  std::unique_ptr<Source> source;
  UdpSocket ctrl;
  ClockSync sync;
  std::atomic<bool> running{true};
  std::atomic<bool> ended{false};

  std::mutex mu;  // guards pending / by_pts / current
  std::map<uint32_t, FrameInfo> pending;     // stamped, not yet at queue output
  // Between queue output and payloader nothing can drop a frame (the encoder is intra-only and
  // 1:1), so frames are matched in order. (PTS cannot be used: GstVideoEncoder shifts it by
  // a 1000 h min-PTS offset.)
  std::deque<FrameInfo> fifo;
  FrameInfo current;                         // frame inside the payloader right now
  bool current_valid = false;
  std::deque<FrameRecord> recent;            // last 4 records (redundant side packets)

  std::atomic<uint32_t> next_id{0};
  std::atomic<int64_t> t_first{0};
  std::atomic<uint64_t> n_stamped{0}, n_qout{0}, n_pay{0}, n_packets{0}, rtp_bytes{0};
  std::atomic<uint64_t> m_stamped{0}, m_qout{0}, m_pay{0}, m_payload_bytes{0}, m_rtp_bytes{0}, m_packets{0};
  std::vector<double> enc_ms, src_to_pay_ms;  // measured frames only (under mu)
  std::vector<uint8_t> scratch;
  FILE* perframe = nullptr;

  bool warmup_at(int64_t t) const {
    return t < t_first.load() + static_cast<int64_t>(warmup_s * 1e9);
  }
};

Sender* g = nullptr;
std::atomic<bool> g_interrupt{false};
void on_sigint(int) { g_interrupt = true; }

GstPadProbeReturn on_stamp(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;
  if (g->ended) return GST_PAD_PROBE_DROP;  // measurement over: let in-flight frames drain
  int64_t t = now_ns();
  int64_t expect = 0;
  g->t_first.compare_exchange_strong(expect, t);
  uint32_t id = g->next_id++;
  if (g->stamp) {
    buf = gst_buffer_make_writable(buf);
    GST_PAD_PROBE_INFO_DATA(info) = buf;
    GstMapInfo m;
    if (gst_buffer_map(buf, &m, GST_MAP_READWRITE)) {
      GstVideoMeta* vm = gst_buffer_get_video_meta(buf);
      int stride = vm ? vm->stride[0] : GST_ROUND_UP_4(g->width * 3);
      stamp_write(m.data, stride, g->width, g->height, id);
      gst_buffer_unmap(buf, &m);
    }
  }
  GST_BUFFER_OFFSET(buf) = id;
  FrameInfo fi;
  fi.id = id;
  fi.t_src = t;
  fi.flags = g->warmup_at(t) ? kFlagWarmup : 0;
  g->n_stamped++;
  if (!fi.flags) g->m_stamped++;
  std::lock_guard<std::mutex> lk(g->mu);
  g->pending[id] = fi;
  while (g->pending.size() > 64) g->pending.erase(g->pending.begin());
  return GST_PAD_PROBE_OK;
}

// First pad after the bounded queue: frame survived the queue. Hash canonical RGB here (raw:
// this IS the payloader input; ffv1: the RGB frame entering the lossless encoder).
GstPadProbeReturn on_qout(GstPad* pad, GstPadProbeInfo* info, gpointer) {
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;
  int64_t t = now_ns();
  uint32_t id = static_cast<uint32_t>(GST_BUFFER_OFFSET(buf));
  FrameInfo fi;
  {
    std::lock_guard<std::mutex> lk(g->mu);
    auto it = g->pending.find(id);
    if (it == g->pending.end()) return GST_PAD_PROBE_OK;
    fi = it->second;
    g->pending.erase(it);
  }
  fi.t_qout = t;
  GstCaps* caps = gst_pad_get_current_caps(pad);
  GstVideoInfo vi;
  if (caps && gst_video_info_from_caps(&vi, caps)) {
    GstVideoFrame f;
    if (gst_video_frame_map(&f, &vi, buf, GST_MAP_READ)) {
      canonical_rgb_hash(&f, &fi.hash, &g->scratch);
      gst_video_frame_unmap(&f);
    }
  }
  if (caps) gst_caps_unref(caps);
  g->n_qout++;
  if (!fi.flags) g->m_qout++;
  std::lock_guard<std::mutex> lk(g->mu);
  g->fifo.push_back(fi);
  while (g->fifo.size() > 64) g->fifo.pop_front();
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn on_pay_sink(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;
  int64_t t = now_ns();
  std::lock_guard<std::mutex> lk(g->mu);
  if (g->fifo.empty()) {
    g->current_valid = false;
    return GST_PAD_PROBE_OK;
  }
  g->current = g->fifo.front();
  g->fifo.pop_front();
  g->current.t_pay = t;
  g->current.payload_bytes = static_cast<uint32_t>(gst_buffer_get_size(buf));
  g->current_valid = true;
  return GST_PAD_PROBE_OK;
}

void count_rtp(GstBuffer* b, bool* first_ts_found, uint32_t* first_ts) {
  g->n_packets++;
  uint64_t sz = gst_buffer_get_size(b);
  g->rtp_bytes += sz;
  if (!*first_ts_found) {
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (gst_rtp_buffer_map(b, GST_MAP_READ, &rtp)) {
      *first_ts = gst_rtp_buffer_get_timestamp(&rtp);
      *first_ts_found = true;
      gst_rtp_buffer_unmap(&rtp);
    }
  }
}

// Payloader output: runs before udpsink sends, so the side record goes out just ahead of the
// frame's packets.
GstPadProbeReturn on_pay_src(GstPad*, GstPadProbeInfo* info, gpointer) {
  bool found = false;
  uint32_t ts = 0;
  uint64_t pk0 = g->n_packets, by0 = g->rtp_bytes;
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    GstBufferList* l = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
    guint n = gst_buffer_list_length(l);
    for (guint i = 0; i < n; ++i) count_rtp(gst_buffer_list_get(l, i), &found, &ts);
  } else if (GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info)) {
    count_rtp(b, &found, &ts);
  }
  std::lock_guard<std::mutex> lk(g->mu);
  if (!g->current_valid || !found) return GST_PAD_PROBE_OK;
  FrameInfo& fi = g->current;
  if (!fi.flags) {
    g->m_packets += g->n_packets - pk0;
    g->m_rtp_bytes += g->rtp_bytes - by0;
  }
  if (fi.sent) return GST_PAD_PROBE_OK;  // later packet batches of the same frame
  fi.sent = true;
  FrameRecord r{};
  r.frame_id = fi.id;
  r.rtp_ts = ts;
  r.flags = fi.flags;
  r.payload_bytes = fi.payload_bytes;
  r.hash = fi.hash;
  r.t_pay_s = fi.t_pay;
  r.t_src_s = fi.t_src;
  if (!g->sync.map(fi.t_pay, &r.t_pay_r, &r.sync_unc_ns)) {
    r.t_pay_r = 0;
    r.sync_unc_ns = 0;
  }
  g->recent.push_back(r);
  while (g->recent.size() > 4) g->recent.pop_front();
  FramePacket p{};
  p.magic = kMagicFrame;
  p.count = static_cast<uint32_t>(g->recent.size());
  for (size_t i = 0; i < g->recent.size(); ++i) p.rec[i] = g->recent[g->recent.size() - 1 - i];
  if (g->ctrl.ok()) g->ctrl.send(&p, sizeof p);
  g->n_pay++;
  if (!fi.flags) {
    g->m_pay++;
    g->m_payload_bytes += fi.payload_bytes;
    g->enc_ms.push_back((fi.t_pay - fi.t_qout) / 1e6);
    g->src_to_pay_ms.push_back((fi.t_pay - fi.t_src) / 1e6);
  }
  if (g->perframe)
    std::fprintf(g->perframe, "%u,%u,%u,%u,%016llx,%lld,%lld,%lld,%lld\n", fi.id, ts, fi.flags,
                 fi.payload_bytes, static_cast<unsigned long long>(fi.hash),
                 static_cast<long long>(fi.t_src), static_cast<long long>(fi.t_qout),
                 static_cast<long long>(fi.t_pay), static_cast<long long>(r.t_pay_r));
  return GST_PAD_PROBE_OK;
}

void ping_loop() {
  uint32_t seq = 0;
  uint8_t buf[256];
  while (g->running) {
    PingPacket p{};
    p.magic = kMagicPing;
    p.seq = ++seq;
    p.t1_s = now_ns();
    g->ctrl.send(&p, sizeof p);
    int64_t deadline = now_ns() + 100'000'000;
    while (g->running && now_ns() < deadline) {
      int n = g->ctrl.recv(buf, sizeof buf, 20);
      if (n == static_cast<int>(sizeof(PingPacket))) {
        int64_t t3 = now_ns();
        PingPacket r;
        std::memcpy(&r, buf, sizeof r);
        if (r.magic == kMagicPing && r.t2_r != 0) g->sync.add(r.t1_s, r.t2_r, t3);
      }
    }
  }
}

void usage() {
  std::puts(
      "sb_sender options:\n"
      "  --source pattern|desktop|capture|gst:<fragment>   (default pattern)\n"
      "  --width 960 --height 540 --fps 60                  transmitted size/rate (RGB 8-bit)\n"
      "  --codec raw|ffv1                                   raw RFC4175 RTP or FFV1 via rtpgstpay\n"
      "  --dest <ip> --bind <local ip> --port 5004 --ctrl-port 5006 --mtu 1400\n"
      "  --warmup 5 --duration 30                           seconds (0 duration = run until killed)\n"
      "  --monitor 0 --capture-api dxgi|wgc --no-cursor     desktop options\n"
      "  --device <n|/dev/videoN>                           capture-card options\n"
      "  --ffv1-slices 16 --ffv1-coder rice|range_def --ffv1-threads 0\n"
      "  --preview [--fullscreen]                           show the stamped frames locally\n"
      "  --preview-monitor 0[,1,...]                        borderless full-screen preview on monitor(s)\n"
      "  --no-stamp                                         do not draw the frame-ID stamp\n"
      "  --csv f --timeseries f --perframe f --label s       outputs\n"
      "  --sndbuf bytes                                     udpsink SO_SNDBUF (default 2 MiB: small, so the OS cannot hide a deep queue)");
}

}  // namespace

int main(int argc, char** argv) {
  Args a(argc, argv);
  if (a.flag("help")) {
    usage();
    return 0;
  }
  timer_init();
  gst_init(&argc, &argv);
  std::signal(SIGINT, on_sigint);

  Sender s;
  g = &s;
  s.codec = a.str("codec", "raw");
  s.dest = a.str("dest", "");
  s.bind = a.str("bind", "");
  s.width = static_cast<int>(a.num("width", 960));
  s.height = static_cast<int>(a.num("height", 540));
  s.fps = static_cast<int>(a.num("fps", 60));
  s.port = static_cast<int>(a.num("port", 5004));
  s.ctrl_port = static_cast<int>(a.num("ctrl-port", 5006));
  s.mtu = static_cast<int>(a.num("mtu", 1400));
  s.warmup_s = a.real("warmup", 5);
  s.duration_s = a.real("duration", 30);
  s.label = a.str("label", "");
  s.csv = a.str("csv", "");
  s.timeseries_path = a.str("timeseries", "");
  s.perframe_path = a.str("perframe", "");
  s.stamp = !a.flag("no-stamp");

  SourceConfig sc;
  sc.kind = a.str("source", "pattern");
  sc.width = s.width;
  sc.height = s.height;
  sc.fps = s.fps;
  sc.monitor = a.str("monitor", "0");
  sc.capture_api = a.str("capture-api", "dxgi");
  sc.show_cursor = !a.flag("no-cursor");
  sc.device = a.str("device", "");
  sc.scene = a.str("scene", "test");
  s.source = make_source(sc);
  if (!s.source) {
    std::fprintf(stderr, "unknown --source %s\n", sc.kind.c_str());
    return 2;
  }

  char caps[256];
  std::snprintf(caps, sizeof caps,
                "video/x-raw,format=RGB,width=%d,height=%d,framerate=%d/1,pixel-aspect-ratio=1/1",
                s.width, s.height, s.fps);
  std::string p = s.source->fragment() + " ! videoconvert n-threads=4 ! videoscale ! ";
  if (!s.source->exact_rate()) p += "videorate drop-only=true ! ";
  p += std::string("capsfilter name=fmt caps=\"") + caps + "\" ! tee name=t ";
  p += "t. ! queue name=sq leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! ";
  std::string pay;
  if (s.codec == "raw") {
    pay = "rtpvrawpay name=pay pt=96 mtu=" + std::to_string(s.mtu);
    p += pay + " ! ";
  } else if (s.codec == "ffv1") {
    p += "videoconvert name=qout ! video/x-raw,format=BGRx ! avenc_ffv1 name=enc gop-size=1 context=0 "
         "thread-type=slice slicecrc=on coder=" + a.str("ffv1-coder", "rice") +
         " slices=" + std::to_string(a.num("ffv1-slices", 16)) +
         " threads=" + a.str("ffv1-threads", "0") + " ! ";
    pay = "rtpgstpay name=pay config-interval=1 mtu=" + std::to_string(s.mtu);
    p += pay + " ! ";
  } else {
    std::fprintf(stderr, "unknown --codec %s\n", s.codec.c_str());
    return 2;
  }
  if (!s.dest.empty()) {
    p += "udpsink name=net host=" + s.dest + " port=" + std::to_string(s.port) +
         " sync=false async=false qos=false buffer-size=" + std::to_string(a.num("sndbuf", 2 << 20));
    if (!s.bind.empty()) p += " bind-address=" + s.bind;
  } else {
    p += "fakesink name=net sync=false async=false";
  }
  PreviewWindows previews;
  if (a.has("preview-monitor")) {
    // Full-screen borderless previews on the given monitors (e.g. "0" or "0,\\.\DISPLAY9").
    if (!previews.create(a.str("preview-monitor"))) {
      std::fprintf(stderr, "cannot create preview on monitor(s) %s; monitors:\n", a.str("preview-monitor").c_str());
      for (auto& m : list_monitors())
        std::fprintf(stderr, "  %s %dx%d at %d,%d%s\n", m.device.c_str(), m.w, m.h, m.x, m.y, m.primary ? " primary" : "");
      return 2;
    }
    for (size_t i = 0; i < previews.count(); ++i)
      p += " t. ! queue leaky=downstream max-size-buffers=1 max-size-bytes=0 max-size-time=0 ! videoconvert ! "
           "d3d11videosink name=pv" + std::to_string(i) + " sync=false force-aspect-ratio=true";
  } else if (a.flag("preview")) {
#ifdef _WIN32
    p += std::string(" t. ! queue leaky=downstream max-size-buffers=1 ! videoconvert ! d3d11videosink sync=false") +
         (a.flag("fullscreen") ? " fullscreen=true" : "");
#else
    p += " t. ! queue leaky=downstream max-size-buffers=1 ! videoconvert ! autovideosink sync=false";
#endif
  }

  std::fprintf(stderr, "[sender] %s | codec=%s %dx%d@%d -> %s:%d (bind %s)\n[sender] pipeline: %s\n",
               s.source->describe().c_str(), s.codec.c_str(), s.width, s.height, s.fps,
               s.dest.empty() ? "(none)" : s.dest.c_str(), s.port, s.bind.empty() ? "any" : s.bind.c_str(),
               p.c_str());

  GError* err = nullptr;
  s.pipeline = gst_parse_launch(p.c_str(), &err);
  if (!s.pipeline || err) {
    std::fprintf(stderr, "pipeline error: %s\n", err ? err->message : "?");
    return 3;
  }
  s.source->attach(s.pipeline);
  previews.attach(s.pipeline, "pv");
  if (previews.count()) std::fprintf(stderr, "[sender] preview on %s\n", previews.describe().c_str());

  auto add_probe = [&](const char* el, const char* padname, GstPadProbeType type, GstPadProbeCallback cb) {
    GstElement* e = gst_bin_get_by_name(GST_BIN(s.pipeline), el);
    if (!e) { std::fprintf(stderr, "no element %s\n", el); return; }
    GstPad* pad = gst_element_get_static_pad(e, padname);
    gst_pad_add_probe(pad, type, cb, nullptr, nullptr);
    gst_object_unref(pad);
    gst_object_unref(e);
  };
  add_probe("fmt", "src", GST_PAD_PROBE_TYPE_BUFFER, on_stamp);
  add_probe(s.codec == "raw" ? "pay" : "qout", "sink", GST_PAD_PROBE_TYPE_BUFFER, on_qout);
  add_probe("pay", "sink", GST_PAD_PROBE_TYPE_BUFFER, on_pay_sink);
  add_probe("pay", "src", static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST), on_pay_src);

  std::thread pinger;
  if (!s.dest.empty()) {
    if (!s.ctrl.open(s.bind, 0) || !s.ctrl.set_peer(s.dest, s.ctrl_port)) {
      std::fprintf(stderr, "cannot open control socket\n");
      return 4;
    }
    pinger = std::thread(ping_loop);
    // Give the clock sync a head start so even the first frames carry a mapped timestamp.
    int64_t until = now_ns() + 1'200'000'000;
    while (now_ns() < until && s.sync.unc_ms() < 0) precise_sleep_until(now_ns() + 50'000'000);
  }
  if (!s.perframe_path.empty()) {
    s.perframe = std::fopen(s.perframe_path.c_str(), "w");
    if (s.perframe) std::fprintf(s.perframe, "frame_id,rtp_ts,flags,payload_bytes,hash,t_src_s,t_qout_s,t_pay_s,t_pay_r\n");
  }

  if (gst_element_set_state(s.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "failed to start pipeline\n");
    return 5;
  }
  s.source->start();

  CpuMeter cpu;
  std::vector<double> cpu_proc, cpu_sys;
  GstBus* bus = gst_element_get_bus(s.pipeline);
  int64_t t_next_log = now_ns() + 1'000'000'000;
  uint64_t last_stamped = 0, last_pay = 0, last_bytes = 0, last_qdrop = 0;
  int rc = 0;
  for (;;) {
    GstMessage* msg = gst_bus_timed_pop_filtered(bus, 50 * GST_MSECOND,
                                                 static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_WARNING));
    if (msg) {
      if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
        GError* e = nullptr;
        gchar* dbg = nullptr;
        bool is_err = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR;
        if (is_err) gst_message_parse_error(msg, &e, &dbg);
        else gst_message_parse_warning(msg, &e, &dbg);
        std::fprintf(stderr, "[sender] %s from %s: %s (%s)\n", is_err ? "ERROR" : "warning",
                     GST_OBJECT_NAME(msg->src), e ? e->message : "", dbg ? dbg : "");
        g_clear_error(&e);
        g_free(dbg);
        if (is_err) rc = 6;
      }
      bool stop = GST_MESSAGE_TYPE(msg) != GST_MESSAGE_WARNING;
      gst_message_unref(msg);
      if (stop) break;
    }
    int64_t now = now_ns();
    if (now >= t_next_log) {
      t_next_log += 1'000'000'000;
      double pp, ps;
      cpu.sample(&pp, &ps);
      uint64_t st = s.n_stamped, pa = s.n_pay, by = s.rtp_bytes;
      uint64_t qdrop = s.n_stamped - s.n_qout;
      bool measuring = s.t_first && !s.warmup_at(now);
      if (measuring) { cpu_proc.push_back(pp); cpu_sys.push_back(ps); }
      double mbps = (by - last_bytes) * 8 / 1e6;
      std::fprintf(stderr, "[sender] %s in=%llu/s sent=%llu/s qdrop=%llu rtp=%.1f Mb/s cpu=%.0f%%(1core) sys=%.0f%% sync=±%.3fms rtt=%.3fms\n",
                   measuring ? "MEAS" : "warm", static_cast<unsigned long long>(st - last_stamped),
                   static_cast<unsigned long long>(pa - last_pay), static_cast<unsigned long long>(qdrop - last_qdrop),
                   mbps, pp, ps, s.sync.unc_ms(), s.sync.best_rtt_ms());
      if (!s.timeseries_path.empty())
        csv_append(s.timeseries_path, {{"t_s", fmt((now - s.t_first) / 1e9, 1)}, {"measuring", measuring ? "1" : "0"},
                                        {"frames_in", std::to_string(st - last_stamped)},
                                        {"frames_sent", std::to_string(pa - last_pay)},
                                        {"queue_drops", std::to_string(qdrop - last_qdrop)},
                                        {"rtp_mbps", fmt(mbps, 2)}, {"cpu_proc_pct_one_core", fmt(pp, 1)},
                                        {"cpu_sys_pct", fmt(ps, 1)}, {"sync_unc_ms", fmt(s.sync.unc_ms(), 3)}});
      last_stamped = st; last_pay = pa; last_bytes = by; last_qdrop = qdrop;
    }
    if (s.duration_s > 0 && s.t_first && now >= s.t_first + static_cast<int64_t>((s.warmup_s + s.duration_s) * 1e9))
      break;
    if (g_interrupt) break;
  }
  gst_object_unref(bus);

  // Stop admitting frames, then wait until every frame that left the queue has been payloaded
  // and the socket buffer has had time to drain, so the last frame is not cut off by shutdown.
  s.ended = true;
  {
    int64_t deadline = now_ns() + 3'000'000'000LL;
    uint64_t last = s.n_pay;
    int64_t stable_since = now_ns();
    while (now_ns() < deadline) {
      precise_sleep_until(now_ns() + 20'000'000);
      bool empty;
      {
        std::lock_guard<std::mutex> lk(s.mu);
        empty = s.fifo.empty();
      }
      if (s.n_pay != last) { last = s.n_pay; stable_since = now_ns(); }
      if (empty && now_ns() - stable_since > 300'000'000) break;
    }
  }
  s.source->stop();
  if (!s.dest.empty()) {
    EndPacket e{};
    e.magic = kMagicEnd;
    e.last_id = s.next_id.load();
    e.frames_measured = static_cast<uint32_t>(s.m_pay.load());
    e.sender_queue_drops = static_cast<uint32_t>(s.m_stamped - s.m_qout);
    e.payload_bytes_measured = s.m_payload_bytes;
    for (int i = 0; i < 5; ++i) {
      s.ctrl.send(&e, sizeof e);
      precise_sleep_until(now_ns() + 20'000'000);
    }
  }
  s.running = false;
  gst_element_set_state(s.pipeline, GST_STATE_NULL);
  if (pinger.joinable()) pinger.join();
  if (s.perframe) std::fclose(s.perframe);

  Summary enc, s2p, cp, cs;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    enc = summarize(s.enc_ms);
    s2p = summarize(s.src_to_pay_ms);
  }
  cp = summarize(cpu_proc);
  cs = summarize(cpu_sys);
  double dur = s.duration_s;
  double payload_mbps = s.m_payload_bytes * 8 / 1e6 / dur;
  double rtp_mbps = s.m_rtp_bytes * 8 / 1e6 / dur;
  // On-wire estimate: RTP + UDP(8) + IPv4(20) + Ethernet header/FCS/preamble/IFG (38) per packet.
  double wire_mbps = (s.m_rtp_bytes + s.m_packets * 66.0) * 8 / 1e6 / dur;
  std::fprintf(stderr,
               "[sender] SUMMARY measured %.0fs: stamped=%llu sent=%llu (%.2f fps) queue_drops=%llu payload=%.1f Mb/s rtp=%.1f Mb/s wire~%.1f Mb/s "
               "encode/queue->pay med=%.2fms p95=%.2fms cpu=%.0f%%(1core) sync_unc=±%.3fms drift=%.2fppm\n",
               dur, static_cast<unsigned long long>(s.m_stamped.load()), static_cast<unsigned long long>(s.m_pay.load()),
               s.m_pay / dur, static_cast<unsigned long long>(s.m_stamped - s.m_qout), payload_mbps, rtp_mbps, wire_mbps,
               enc.median, enc.p95, cp.median, s.sync.unc_ms(), s.sync.drift_ppm());
  if (!s.csv.empty()) {
    csv_append(s.csv, {{"time", wall_clock_iso()}, {"label", s.label}, {"source", s.source->describe()},
                       {"codec", s.codec}, {"width", std::to_string(s.width)}, {"height", std::to_string(s.height)},
                       {"fps_nominal", std::to_string(s.fps)}, {"duration_s", fmt(dur, 1)},
                       {"frames_stamped", std::to_string(s.m_stamped.load())},
                       {"frames_sent", std::to_string(s.m_pay.load())}, {"fps_sent", fmt(s.m_pay / dur, 2)},
                       {"sender_queue_drops", std::to_string(s.m_stamped - s.m_qout)},
                       {"payload_mbps", fmt(payload_mbps, 2)}, {"rtp_mbps", fmt(rtp_mbps, 2)},
                       {"wire_mbps_est", fmt(wire_mbps, 2)}, {"packets", std::to_string(s.m_packets.load())},
                       {"enc_ms_med", fmt(enc.median)}, {"enc_ms_p95", fmt(enc.p95)},
                       {"src_to_pay_ms_med", fmt(s2p.median)}, {"src_to_pay_ms_p95", fmt(s2p.p95)},
                       {"cpu_proc_pct_one_core_med", fmt(cp.median, 1)}, {"cpu_sys_pct_med", fmt(cs.median, 1)},
                       {"sync_unc_ms", fmt(s.sync.unc_ms(), 3)}, {"sync_best_rtt_ms", fmt(s.sync.best_rtt_ms(), 3)},
                       {"clock_drift_ppm", fmt(s.sync.drift_ppm(), 2)}, {"sync_pings_rejected", std::to_string(s.sync.rejected())}, {"exit_code", std::to_string(rc)},
                       {"args", a.dump()}});
  }
  gst_object_unref(s.pipeline);
  return rc;
}
