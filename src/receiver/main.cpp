// sb_receiver: receive the benchmark stream, reconstruct frames, verify them against the
// sender's side-channel hashes, optionally show them on HDMI, and log statistics.
//
//   --mode raw  --rtp-path app : udpsrc -> appsink -> built-in RFC 4175 reassembler (exact
//                                 per-line completeness) -> verify -> appsrc display pipeline
//   --mode raw  --rtp-path gst : udpsrc -> rtpvrawdepay -> tee(verify appsink, display)
//   --mode ffv1                : udpsrc -> rtpgstdepay -> queue(leaky) -> avdec_ffv1 -> tee(...)
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../common/canon.h"
#include "../common/proto.h"
#include "../common/stamp.h"
#include "../common/util.h"
#include "../common/xxhash64.h"

using namespace sb;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct TsInfo {
  int64_t t_first = 0, t_complete = 0;
  uint32_t packets = 0;
  bool damaged = false;    // a sequence gap touched this frame
  bool complete = false;   // all lines present (app path) / depayloader produced it (gst path)
  bool rx_dropped = false; // dropped inside the receiver (no free buffer)
};

struct Verified {
  bool ok = false, bad = false;
  int64_t t_complete = 0, t_first = 0, t_verified = 0, t_display = 0, t_dec_in = 0, t_dec_out = 0;
};

struct Slot {
  std::vector<uint8_t> data;
  std::vector<uint32_t> line_bytes;
  int lines_complete = 0;
  uint32_t rtp_ts = 0;
  uint32_t packets = 0;
  int64_t t_first = 0, t_last = 0;
  bool complete = false;
};

struct Receiver {
  // config
  std::string mode = "raw", rtp_path = "app", bind, display = "none", label, csv, ts_path, pf_path;
  int width = 960, height = 540, fps = 60, port = 5004, ctrl_port = 5006, slots_n = 6;
  bool fullscreen = true;

  // side channel / accounting
  UdpSocket ctrl;
  std::mutex mu;
  std::unordered_map<uint32_t, FrameRecord> rec_by_id;
  std::unordered_map<uint32_t, uint32_t> id_by_ts;
  std::unordered_map<uint32_t, TsInfo> ts_info;
  std::unordered_map<uint32_t, Verified> ver;
  std::unordered_map<uint32_t, int> seen_count;
  std::atomic<bool> end_seen{false};
  EndPacket end_pkt{};
  std::atomic<int64_t> t_end_seen{0};

  // counters
  std::atomic<uint64_t> packets{0}, bytes{0}, lost{0}, dup_or_reorder{0}, stamp_bad{0}, unknown{0},
      duplicates{0}, rx_drop_nobuf{0}, warm_frames{0}, frames_out{0}, display_frames{0},
      display_pushed{0}, ts_mismatch{0}, verified_ok_total{0}, verified_bad_total{0};
  std::atomic<int64_t> t_last_packet{0};

  // raw app path
  std::mutex pool_mu;
  std::vector<std::unique_ptr<Slot>> slots;
  std::vector<Slot*> free_slots;
  Slot* cur = nullptr;
  bool have_seq = false;
  uint16_t last_seq = 0;
  std::deque<Slot*> work;
  std::mutex work_mu;
  std::condition_variable work_cv;
  GstElement* disp_src = nullptr;

  // gst path
  uint32_t gst_cur_ts = 0;
  bool gst_have_ts = false;
  std::mutex pts_mu;
  std::map<GstClockTime, std::pair<uint32_t, int64_t>> complete_by_pts;  // pts -> (rtp_ts, t)
  std::map<GstClockTime, int64_t> dec_in_by_pts, dec_out_by_pts;
  GstElement* decq = nullptr;

  std::vector<uint8_t> scratch;
  FILE* pf = nullptr;

  // per-second series
  std::atomic<uint64_t> sec_ok{0}, sec_bad{0};
  std::mutex sec_mu;
  std::vector<double> sec_lat;
};

Receiver* R = nullptr;

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

void note_seq(uint16_t seq) {
  if (!R->have_seq) {
    R->have_seq = true;
    R->last_seq = seq;
    return;
  }
  uint16_t d = static_cast<uint16_t>(seq - static_cast<uint16_t>(R->last_seq + 1));
  if (d == 0) {
    R->last_seq = seq;
  } else if (d < 0x8000) {
    R->lost += d;
    R->last_seq = seq;
  } else {
    R->dup_or_reorder++;
  }
}

// ------------------------------------------------------------------ verification (both paths)
// Called with a reconstructed frame. Returns true if it matched the sender's hash.
void verify_frame(const uint8_t* data, int stride, int bpp, GstVideoFormat fmt, bool complete,
                  uint32_t rtp_ts, bool ts_known, int64_t t_first, int64_t t_complete,
                  int64_t t_dec_in, int64_t t_dec_out) {
  uint32_t sid = 0;
  bool stamp_ok = false;
  int code_rows = stamp_code_rows(R->width, R->height);
  (void)code_rows;
  stamp_ok = stamp_read(data, stride, R->width, R->height, &sid, bpp);

  FrameRecord rec{};
  bool have_rec = false;
  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lk(R->mu);
    if (ts_known) {
      auto it = R->id_by_ts.find(rtp_ts);
      if (it != R->id_by_ts.end()) { id = it->second; have_rec = true; }
    }
    if (!have_rec && stamp_ok) {
      if (R->rec_by_id.count(sid)) { id = sid; have_rec = true; }
    }
    if (have_rec) rec = R->rec_by_id[id];
  }
  if (!have_rec) {
    if (!stamp_ok) R->stamp_bad++;
    else R->unknown++;
    return;
  }
  if (rec.flags & kFlagWarmup) {
    R->warm_frames++;
    return;
  }
  if (stamp_ok && sid != id) R->ts_mismatch++;
  uint64_t h = 0;
  bool hashed = complete && canonical_rgb_hash_raw(data, stride, R->width, R->height, fmt, &h, &R->scratch);
  int64_t t_ver = now_ns();
  bool ok = hashed && h == rec.hash;
  {
    std::lock_guard<std::mutex> lk(R->mu);
    int& seen = R->seen_count[id];
    if (++seen > 1) {
      R->duplicates++;
      return;
    }
    Verified& v = R->ver[id];
    v.ok = ok;
    v.bad = complete && !ok;
    v.t_first = t_first;
    v.t_complete = t_complete;
    v.t_verified = t_ver;
    v.t_dec_in = t_dec_in;
    v.t_dec_out = t_dec_out;
  }
  if (ok) {
    R->verified_ok_total++;
    R->sec_ok++;
    if (rec.t_pay_r) {
      std::lock_guard<std::mutex> lk(R->sec_mu);
      R->sec_lat.push_back((t_complete - rec.t_pay_r) / 1e6);
    }
  } else if (complete) {
    R->verified_bad_total++;
    R->sec_bad++;
  }
  if (R->pf)
    std::fprintf(R->pf, "%u,%u,%d,%d,%d,%016llx,%016llx,%lld,%lld,%lld,%lld,%lld,%lld\n", id, rtp_ts,
                 complete ? 1 : 0, ok ? 1 : 0, stamp_ok ? 1 : 0, static_cast<unsigned long long>(rec.hash),
                 static_cast<unsigned long long>(h), static_cast<long long>(rec.t_pay_r), static_cast<long long>(t_first),
                 static_cast<long long>(t_complete), static_cast<long long>(t_dec_in), static_cast<long long>(t_dec_out),
                 static_cast<long long>(t_ver));
}


// ------------------------------------------------------------------ raw app path
void slot_release(gpointer p) {
  Slot* s = static_cast<Slot*>(p);
  std::lock_guard<std::mutex> lk(R->pool_mu);
  R->free_slots.push_back(s);
}

Slot* slot_acquire() {
  std::lock_guard<std::mutex> lk(R->pool_mu);
  if (R->free_slots.empty()) return nullptr;
  Slot* s = R->free_slots.back();
  R->free_slots.pop_back();
  return s;
}

void finalize(Slot* s) {
  s->complete = s->lines_complete == R->height;
  {
    std::lock_guard<std::mutex> lk(R->mu);
    TsInfo& ti = R->ts_info[s->rtp_ts];
    ti.complete = s->complete;
    ti.t_complete = s->t_last;
    if (!s->complete) ti.damaged = true;
  }
  R->frames_out++;
  {
    std::lock_guard<std::mutex> lk(R->work_mu);
    R->work.push_back(s);
  }
  R->work_cv.notify_one();
}

GstFlowReturn on_rtp_sample(GstAppSink* sink, gpointer) {
  GstSample* smp = gst_app_sink_pull_sample(sink);
  if (!smp) return GST_FLOW_EOS;
  GstBuffer* b = gst_sample_get_buffer(smp);
  GstMapInfo m;
  int64_t t = now_ns();
  if (b && gst_buffer_map(b, &m, GST_MAP_READ)) {
    const uint8_t* d = m.data;
    size_t n = m.size;
    R->packets++;
    R->bytes += n;
    R->t_last_packet = t;
    if (n >= 14 && (d[0] >> 6) == 2) {
      int cc = d[0] & 0x0F;
      bool ext = d[0] & 0x10;
      bool marker = d[1] & 0x80;
      uint16_t seq = be16(d + 2);
      uint32_t ts = be32(d + 4);
      size_t off = 12 + 4 * cc;
      if (ext && off + 4 <= n) off += 4 + 4 * be16(d + off + 2);
      note_seq(seq);
      if (R->cur && R->cur->rtp_ts != ts) {  // marker of the previous frame was lost
        finalize(R->cur);
        R->cur = nullptr;
      }
      bool known_new = false;
      if (!R->cur) {
        std::lock_guard<std::mutex> lk(R->mu);
        TsInfo& ti = R->ts_info[ts];
        known_new = ti.packets == 0;
        ti.packets++;
        if (ti.t_first == 0) ti.t_first = t;
        if (ti.rx_dropped) known_new = false;
      } else {
        R->cur->packets++;
      }
      if (!R->cur && known_new) {
        Slot* s = slot_acquire();
        if (s) {
          s->rtp_ts = ts;
          s->packets = 1;
          s->t_first = t;
          s->lines_complete = 0;
          std::fill(s->line_bytes.begin(), s->line_bytes.end(), 0u);
          R->cur = s;
        } else {
          R->rx_drop_nobuf++;
          std::lock_guard<std::mutex> lk(R->mu);
          R->ts_info[ts].rx_dropped = true;
        }
      }
      if (R->cur && off + 2 <= n) {
        Slot* s = R->cur;
        const size_t row = static_cast<size_t>(R->width) * 3;
        size_t p = off + 2;  // skip extended sequence number
        struct Hdr { uint16_t len, line, offs; };
        Hdr hdr[64];
        int nh = 0;
        bool cont = true;
        while (cont && p + 6 <= n && nh < 64) {
          hdr[nh].len = be16(d + p);
          hdr[nh].line = be16(d + p + 2) & 0x7FFF;
          uint16_t o = be16(d + p + 4);
          hdr[nh].offs = o & 0x7FFF;
          cont = o & 0x8000;
          ++nh;
          p += 6;
        }
        for (int i = 0; i < nh && p < n; ++i) {
          size_t len = std::min<size_t>(hdr[i].len, n - p);
          size_t dst = static_cast<size_t>(hdr[i].line) * row + static_cast<size_t>(hdr[i].offs) * 3;
          if (hdr[i].line < R->height && dst + len <= s->data.size()) {
            std::memcpy(s->data.data() + dst, d + p, len);
            uint32_t& lb = s->line_bytes[hdr[i].line];
            bool was = lb >= row;
            lb += static_cast<uint32_t>(len);
            if (!was && lb >= row) s->lines_complete++;
          }
          p += hdr[i].len;
        }
        s->t_last = t;
        if (marker) {
          finalize(s);
          R->cur = nullptr;
        }
      } else if (marker && R->cur) {
        finalize(R->cur);
        R->cur = nullptr;
      }
    }
    gst_buffer_unmap(b, &m);
  }
  gst_sample_unref(smp);
  return GST_FLOW_OK;
}

void worker_loop() {
  const int stride = R->width * 3;
  while (!g_stop) {
    Slot* s = nullptr;
    {
      std::unique_lock<std::mutex> lk(R->work_mu);
      R->work_cv.wait_for(lk, std::chrono::milliseconds(100), [] { return !R->work.empty() || g_stop.load(); });
      if (R->work.empty()) continue;
      s = R->work.front();
      R->work.pop_front();
    }
    verify_frame(s->data.data(), stride, 3, GST_VIDEO_FORMAT_RGB, s->complete, s->rtp_ts, true, s->t_first,
                 s->t_last, 0, 0);
    if (R->disp_src && s->complete) {
      GstBuffer* buf = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY, s->data.data(), s->data.size(), 0,
                                                   s->data.size(), s, slot_release);
      gsize offs[4] = {0};
      gint strides[4] = {stride};
      gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_RGB, R->width, R->height, 1,
                                     offs, strides);
      gst_app_src_push_buffer(GST_APP_SRC(R->disp_src), buf);
      R->display_pushed++;
    } else {
      slot_release(s);
    }
  }
}

// ------------------------------------------------------------------ gst path probes
GstPadProbeReturn on_depay_sink(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!b) return GST_PAD_PROBE_OK;
  int64_t t = now_ns();
  R->packets++;
  R->bytes += gst_buffer_get_size(b);
  R->t_last_packet = t;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  if (gst_rtp_buffer_map(b, GST_MAP_READ, &rtp)) {
    uint16_t seq = gst_rtp_buffer_get_seq(&rtp);
    uint32_t ts = gst_rtp_buffer_get_timestamp(&rtp);
    gst_rtp_buffer_unmap(&rtp);
    uint64_t lost0 = R->lost;
    note_seq(seq);
    bool gap = R->lost != lost0;
    std::lock_guard<std::mutex> lk(R->mu);
    TsInfo& ti = R->ts_info[ts];
    if (ti.t_first == 0) ti.t_first = t;
    ti.packets++;
    if (gap) {
      ti.damaged = true;
      if (R->gst_have_ts && R->gst_cur_ts != ts) R->ts_info[R->gst_cur_ts].damaged = true;
    }
    R->gst_cur_ts = ts;
    R->gst_have_ts = true;
  }
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn on_depay_src(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!b) return GST_PAD_PROBE_OK;
  int64_t t = now_ns();
  uint32_t ts;
  {
    std::lock_guard<std::mutex> lk(R->mu);
    ts = R->gst_cur_ts;
    TsInfo& ti = R->ts_info[ts];
    ti.complete = true;
    ti.t_complete = t;
  }
  R->frames_out++;
  std::lock_guard<std::mutex> lk(R->pts_mu);
  R->complete_by_pts[GST_BUFFER_PTS(b)] = {ts, t};
  while (R->complete_by_pts.size() > 256) R->complete_by_pts.erase(R->complete_by_pts.begin());
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn on_dec_in(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!b) return GST_PAD_PROBE_OK;
  std::lock_guard<std::mutex> lk(R->pts_mu);
  R->dec_in_by_pts[GST_BUFFER_PTS(b)] = now_ns();
  while (R->dec_in_by_pts.size() > 256) R->dec_in_by_pts.erase(R->dec_in_by_pts.begin());
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn on_dec_out(GstPad*, GstPadProbeInfo* info, gpointer) {
  GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!b) return GST_PAD_PROBE_OK;
  std::lock_guard<std::mutex> lk(R->pts_mu);
  R->dec_out_by_pts[GST_BUFFER_PTS(b)] = now_ns();
  while (R->dec_out_by_pts.size() > 256) R->dec_out_by_pts.erase(R->dec_out_by_pts.begin());
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn on_display_in(GstPad*, GstPadProbeInfo* info, gpointer) {
  if (GST_PAD_PROBE_INFO_BUFFER(info)) R->display_frames++;
  return GST_PAD_PROBE_OK;
}

GstFlowReturn on_verify_sample(GstAppSink* sink, gpointer) {
  GstSample* smp = gst_app_sink_pull_sample(sink);
  if (!smp) return GST_FLOW_EOS;
  GstBuffer* b = gst_sample_get_buffer(smp);
  GstCaps* caps = gst_sample_get_caps(smp);
  GstVideoInfo vi;
  if (b && caps && gst_video_info_from_caps(&vi, caps)) {
    GstVideoFrame f;
    if (gst_video_frame_map(&f, &vi, b, GST_MAP_READ)) {
      GstClockTime pts = GST_BUFFER_PTS(b);
      uint32_t ts = 0;
      bool ts_known = false;
      int64_t t_c = 0, t_in = 0, t_out = 0;
      {
        std::lock_guard<std::mutex> lk(R->pts_mu);
        auto it = R->complete_by_pts.find(pts);
        if (it != R->complete_by_pts.end()) { ts = it->second.first; t_c = it->second.second; ts_known = true; }
        auto a = R->dec_in_by_pts.find(pts);
        if (a != R->dec_in_by_pts.end()) t_in = a->second;
        auto o = R->dec_out_by_pts.find(pts);
        if (o != R->dec_out_by_pts.end()) t_out = o->second;
      }
      int64_t t_first = 0;
      if (ts_known) {
        std::lock_guard<std::mutex> lk(R->mu);
        t_first = R->ts_info[ts].t_first;
      }
      int bpp = GST_VIDEO_INFO_COMP_PSTRIDE(&vi, 0);
      verify_frame(static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&f, 0)), GST_VIDEO_FRAME_PLANE_STRIDE(&f, 0),
                   bpp, GST_VIDEO_INFO_FORMAT(&vi), true, ts, ts_known, t_first, t_c ? t_c : now_ns(), t_in, t_out);
      gst_video_frame_unmap(&f);
    }
  }
  gst_sample_unref(smp);
  return GST_FLOW_OK;
}

// ------------------------------------------------------------------ side channel
void ctrl_loop() {
  uint8_t buf[2048];
  unsigned char from[32];
  while (!g_stop) {
    int fl = sizeof from;
    int n = R->ctrl.recv(buf, sizeof buf, 100, from, &fl);
    if (n < 4) continue;
    uint32_t magic;
    std::memcpy(&magic, buf, 4);
    if (magic == kMagicPing && n == static_cast<int>(sizeof(PingPacket))) {
      PingPacket p;
      std::memcpy(&p, buf, sizeof p);
      p.t2_r = now_ns();
      R->ctrl.send_to(&p, sizeof p, from, fl);
    } else if (magic == kMagicFrame && n == static_cast<int>(sizeof(FramePacket))) {
      FramePacket fp;
      std::memcpy(&fp, buf, sizeof fp);
      std::lock_guard<std::mutex> lk(R->mu);
      for (uint32_t i = 0; i < fp.count && i < 4; ++i) {
        const FrameRecord& r = fp.rec[i];
        if (R->rec_by_id.emplace(r.frame_id, r).second) R->id_by_ts[r.rtp_ts] = r.frame_id;
      }
    } else if (magic == kMagicEnd && n == static_cast<int>(sizeof(EndPacket))) {
      if (!R->end_seen) {
        std::memcpy(&R->end_pkt, buf, sizeof(EndPacket));
        R->t_end_seen = now_ns();
        R->end_seen = true;
      }
    }
  }
}

std::string display_sink(const std::string& d, bool fullscreen) {
  if (d == "wayland") return std::string("waylandsink name=dsink sync=false") + (fullscreen ? " fullscreen=true" : "");
  if (d == "kms") return "kmssink name=dsink sync=false force-modesetting=false";
  if (d == "gl") return "glimagesink name=dsink sync=false";
  if (d == "fake") return "fakesink name=dsink sync=false";
  if (d == "auto") return "autovideosink name=dsink sync=false";
  return "";
}

void usage() {
  std::puts(
      "sb_receiver options:\n"
      "  --mode raw|ffv1 --rtp-path app|gst (raw only) --width 960 --height 540 --fps 60\n"
      "  --bind <local ip> --port 5004 --ctrl-port 5006 --rcvbuf bytes (default 32 MiB)\n"
      "  --display none|wayland|kms|gl|fake|auto [--windowed]\n"
      "  --csv f --timeseries f --perframe f --label s\n"
      "  --start-timeout 30 --idle-timeout 5 --max-run 0");
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
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  Receiver r;
  R = &r;
  r.mode = a.str("mode", "raw");
  r.rtp_path = r.mode == "raw" ? a.str("rtp-path", "app") : "gst";
  r.bind = a.str("bind", "");
  r.display = a.str("display", "none");
  r.fullscreen = !a.flag("windowed");
  r.width = static_cast<int>(a.num("width", 960));
  r.height = static_cast<int>(a.num("height", 540));
  r.fps = static_cast<int>(a.num("fps", 60));
  r.port = static_cast<int>(a.num("port", 5004));
  r.ctrl_port = static_cast<int>(a.num("ctrl-port", 5006));
  r.label = a.str("label", "");
  r.csv = a.str("csv", "");
  r.ts_path = a.str("timeseries", "");
  r.pf_path = a.str("perframe", "");
  int64_t rcvbuf = a.num("rcvbuf", 32 << 20);

  if (!r.ctrl.open(r.bind, r.ctrl_port)) {
    std::fprintf(stderr, "cannot bind control port %s:%d\n", r.bind.c_str(), r.ctrl_port);
    return 4;
  }
  std::thread ctrl_th(ctrl_loop);

  std::string udp = "udpsrc name=src port=" + std::to_string(r.port) + " buffer-size=" + std::to_string(rcvbuf) +
                    " do-timestamp=true" + (r.bind.empty() ? "" : " address=" + r.bind);
  std::string dsink = display_sink(r.display, r.fullscreen);
  std::string p, dp;
  GstVideoFormat vfmt = GST_VIDEO_FORMAT_RGB;
  if (r.mode == "raw" && r.rtp_path == "app") {
    p = udp + " caps=application/x-rtp ! appsink name=rtpsink sync=false async=false emit-signals=false max-buffers=0 drop=false";
    if (!dsink.empty())
      dp = "appsrc name=dsrc is-live=true format=time do-timestamp=true block=false max-buffers=2 leaky-type=downstream "
           "caps=video/x-raw,format=RGB,width=" + std::to_string(r.width) + ",height=" + std::to_string(r.height) +
           ",framerate=" + std::to_string(r.fps) + "/1 ! queue leaky=downstream max-size-buffers=2 max-size-bytes=0 "
           "max-size-time=0 ! videoconvert ! " + dsink;
  } else {
    std::string head;
    if (r.mode == "raw") {
      head = udp + " caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=RAW,sampling=RGB,"
                   "depth=(string)8,width=(string)" + std::to_string(r.width) + ",height=(string)" +
             std::to_string(r.height) + ",payload=96\" ! rtpvrawdepay name=depay ";
    } else if (r.mode == "ffv1") {
      head = udp + " caps=\"application/x-rtp,media=application,clock-rate=90000,encoding-name=X-GST,payload=96\" ! "
                   "rtpgstdepay name=depay ! queue name=decq leaky=downstream max-size-buffers=3 max-size-bytes=0 "
                   "max-size-time=0 ! avdec_ffv1 name=dec thread-type=slice max-threads=" +
             a.str("dec-threads", "4") + " ";
      vfmt = GST_VIDEO_FORMAT_BGRx;
    } else {
      std::fprintf(stderr, "unknown --mode\n");
      return 2;
    }
    p = head + "! tee name=t t. ! queue name=vq leaky=downstream max-size-buffers=3 max-size-bytes=0 max-size-time=0 ! "
               "appsink name=vsink sync=false async=false max-buffers=3 drop=true";
    if (!dsink.empty())
      p += " t. ! queue name=dq leaky=downstream max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! videoconvert ! " + dsink;
  }
  (void)vfmt;
  std::fprintf(stderr, "[receiver] mode=%s path=%s %dx%d@%d bind=%s:%d display=%s\n[receiver] pipeline: %s\n%s%s\n",
               r.mode.c_str(), r.rtp_path.c_str(), r.width, r.height, r.fps, r.bind.empty() ? "any" : r.bind.c_str(),
               r.port, r.display.c_str(), p.c_str(), dp.empty() ? "" : "[receiver] display: ", dp.c_str());

  GError* err = nullptr;
  GstElement* pipe = gst_parse_launch(p.c_str(), &err);
  if (!pipe || err) {
    std::fprintf(stderr, "pipeline error: %s\n", err ? err->message : "?");
    return 3;
  }
  GstElement* dpipe = nullptr;
  if (!dp.empty()) {
    dpipe = gst_parse_launch(dp.c_str(), &err);
    if (!dpipe || err) {
      std::fprintf(stderr, "display pipeline error: %s\n", err ? err->message : "?");
      return 3;
    }
    r.disp_src = gst_bin_get_by_name(GST_BIN(dpipe), "dsrc");
  }
  auto probe = [](GstElement* bin, const char* el, const char* pad, GstPadProbeCallback cb) {
    GstElement* e = gst_bin_get_by_name(GST_BIN(bin), el);
    if (!e) return;
    GstPad* pd = gst_element_get_static_pad(e, pad);
    gst_pad_add_probe(pd, GST_PAD_PROBE_TYPE_BUFFER, cb, nullptr, nullptr);
    gst_object_unref(pd);
    gst_object_unref(e);
  };

  std::thread worker;
  if (r.rtp_path == "app") {
    r.slots_n = static_cast<int>(a.num("slots", 6));
    for (int i = 0; i < r.slots_n; ++i) {
      auto s = std::make_unique<Slot>();
      s->data.assign(static_cast<size_t>(r.width) * r.height * 3, 0);
      s->line_bytes.assign(r.height, 0);
      r.free_slots.push_back(s.get());
      r.slots.push_back(std::move(s));
    }
    GstElement* as = gst_bin_get_by_name(GST_BIN(pipe), "rtpsink");
    GstAppSinkCallbacks cb{};
    cb.new_sample = on_rtp_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(as), &cb, nullptr, nullptr);
    gst_object_unref(as);
    worker = std::thread(worker_loop);
    if (dpipe) probe(dpipe, "dsink", "sink", on_display_in);
  } else {
    probe(pipe, "depay", "sink", on_depay_sink);
    probe(pipe, "depay", "src", on_depay_src);
    probe(pipe, "dec", "sink", on_dec_in);
    probe(pipe, "dec", "src", on_dec_out);
    probe(pipe, "dsink", "sink", on_display_in);
    GstElement* vs = gst_bin_get_by_name(GST_BIN(pipe), "vsink");
    GstAppSinkCallbacks cb{};
    cb.new_sample = on_verify_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(vs), &cb, nullptr, nullptr);
    gst_object_unref(vs);
    r.decq = gst_bin_get_by_name(GST_BIN(pipe), "decq");
  }
  if (!r.pf_path.empty()) {
    r.pf = std::fopen(r.pf_path.c_str(), "w");
    if (r.pf) std::fprintf(r.pf, "frame_id,rtp_ts,complete,ok,stamp_ok,hash_tx,hash_rx,t_pay_r,t_first,t_complete,t_dec_in,t_dec_out,t_verified\n");
  }

  if (dpipe) gst_element_set_state(dpipe, GST_STATE_PLAYING);
  if (gst_element_set_state(pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    std::fprintf(stderr, "failed to start pipeline\n");
    return 5;
  }

  const int64_t start = now_ns();
  const int64_t start_timeout = static_cast<int64_t>(a.real("start-timeout", 30) * 1e9);
  const int64_t idle_timeout = static_cast<int64_t>(a.real("idle-timeout", 5) * 1e9);
  const int64_t max_run = static_cast<int64_t>(a.real("max-run", 0) * 1e9);
  CpuMeter cpu;
  std::vector<double> cpu_p, cpu_s;
  GstBus* bus = gst_element_get_bus(pipe);
  GstBus* dbus = dpipe ? gst_element_get_bus(dpipe) : nullptr;
  int64_t t_next = now_ns() + 1'000'000'000;
  uint64_t lp = 0, lb = 0, ll = 0, ld = 0;
  int rc = 0;
  int64_t t_meas_first = 0;
  while (!g_stop) {
    for (GstBus* bb : {bus, dbus}) {
      if (!bb) continue;
      GstMessage* msg = gst_bus_timed_pop_filtered(bb, 20 * GST_MSECOND,
                                                   static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
      if (!msg) continue;
      GError* e = nullptr;
      gchar* dbg = nullptr;
      bool is_err = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR;
      if (is_err) gst_message_parse_error(msg, &e, &dbg);
      else gst_message_parse_warning(msg, &e, &dbg);
      std::fprintf(stderr, "[receiver] %s from %s: %s (%s)\n", is_err ? "ERROR" : "warning", GST_OBJECT_NAME(msg->src),
                   e ? e->message : "", dbg ? dbg : "");
      g_clear_error(&e);
      g_free(dbg);
      gst_message_unref(msg);
      if (is_err) { rc = 6; g_stop = true; }
    }
    int64_t now = now_ns();
    if (now >= t_next) {
      t_next += 1'000'000'000;
      double pp, ps;
      cpu.sample(&pp, &ps);
      bool measuring;
      {
        std::lock_guard<std::mutex> lk(r.mu);
        measuring = false;
        for (auto& [id, rec] : r.rec_by_id)
          if (!(rec.flags & kFlagWarmup)) { measuring = true; break; }
      }
      if (measuring && !r.end_seen) {
        if (!t_meas_first) t_meas_first = now;
        cpu_p.push_back(pp);
        cpu_s.push_back(ps);
      }
      uint64_t pk = r.packets, by = r.bytes, lo = r.lost, dd = r.display_frames;
      std::vector<double> lat;
      {
        std::lock_guard<std::mutex> lk(r.sec_mu);
        lat.swap(r.sec_lat);
      }
      Summary ls = summarize(lat);
      uint64_t ok = r.sec_ok.exchange(0), bad = r.sec_bad.exchange(0);
      int qlev = 0;
      if (r.decq) {
        guint lvl = 0;
        g_object_get(r.decq, "current-level-buffers", &lvl, nullptr);
        qlev = static_cast<int>(lvl);
      } else {
        std::lock_guard<std::mutex> lk(r.work_mu);
        qlev = static_cast<int>(r.work.size());
      }
      double mbps = (by - lb) * 8 / 1e6;
      std::fprintf(stderr, "[receiver] %s ok=%llu/s bad=%llu pkts=%llu lost=%llu rtp=%.1f Mb/s lat(pay->rx) med=%.2fms p95=%.2fms disp=%llu/s q=%d cpu=%.0f%%(1core) sys=%.0f%%\n",
                   measuring ? "MEAS" : "warm", static_cast<unsigned long long>(ok), static_cast<unsigned long long>(bad),
                   static_cast<unsigned long long>(pk - lp), static_cast<unsigned long long>(lo - ll), mbps, ls.median, ls.p95,
                   static_cast<unsigned long long>(dd - ld), qlev, pp, ps);
      if (!r.ts_path.empty())
        csv_append(r.ts_path, {{"t_s", fmt((now - start) / 1e9, 1)}, {"measuring", measuring ? "1" : "0"},
                               {"frames_ok", std::to_string(ok)}, {"frames_bad", std::to_string(bad)},
                               {"packets", std::to_string(pk - lp)}, {"lost", std::to_string(lo - ll)},
                               {"rtp_mbps", fmt(mbps, 2)}, {"lat_pay_rx_med_ms", fmt(ls.median)},
                               {"lat_pay_rx_p95_ms", fmt(ls.p95)}, {"display_fps", std::to_string(dd - ld)},
                               {"queue_level", std::to_string(qlev)}, {"cpu_proc_pct_one_core", fmt(pp, 1)},
                               {"cpu_sys_pct", fmt(ps, 1)}});
      lp = pk; lb = by; ll = lo; ld = dd;
    }
    if (r.end_seen && now - r.t_end_seen > 1'000'000'000) break;
    int64_t lastp = r.t_last_packet;
    if (!lastp && now - start > start_timeout) { std::fprintf(stderr, "[receiver] no packets, giving up\n"); rc = 7; break; }
    if (lastp && now - lastp > idle_timeout && !r.end_seen) { std::fprintf(stderr, "[receiver] stream idle, finishing\n"); break; }
    if (max_run && now - start > max_run) break;
  }
  g_stop = true;
  gst_element_set_state(pipe, GST_STATE_NULL);
  if (dpipe) gst_element_set_state(dpipe, GST_STATE_NULL);
  if (worker.joinable()) worker.join();
  if (ctrl_th.joinable()) ctrl_th.join();
  if (r.pf) std::fclose(r.pf);

  // ------------------------------------------------------------ final accounting
  uint64_t expected = 0, ok = 0, corrupted = 0, incomplete = 0, never = 0, rx_dropped = 0;
  std::vector<double> l_first, l_complete, l_verified, l_dec, l_src_ver, unc;
  {
    std::lock_guard<std::mutex> lk(r.mu);
    for (auto& [id, rec] : r.rec_by_id) {
      if (rec.flags & kFlagWarmup) continue;
      ++expected;
      auto v = r.ver.find(id);
      if (v != r.ver.end() && v->second.ok) {
        ++ok;
        const Verified& x = v->second;
        if (rec.t_pay_r) {
          int64_t off = rec.t_pay_r - rec.t_pay_s;
          if (x.t_first) l_first.push_back((x.t_first - rec.t_pay_r) / 1e6);
          l_complete.push_back((x.t_complete - rec.t_pay_r) / 1e6);
          l_verified.push_back((x.t_verified - rec.t_pay_r) / 1e6);
          l_src_ver.push_back((x.t_verified - (rec.t_src_s + off)) / 1e6);
          unc.push_back(rec.sync_unc_ns / 1e6);
        }
        if (x.t_dec_in && x.t_dec_out) l_dec.push_back((x.t_dec_out - x.t_dec_in) / 1e6);
        continue;
      }
      if (v != r.ver.end() && v->second.bad) { ++corrupted; continue; }
      auto ti = r.ts_info.find(rec.rtp_ts);
      if (ti == r.ts_info.end() || ti->second.packets == 0) { ++never; continue; }
      if (ti->second.rx_dropped) { ++rx_dropped; continue; }
      if (ti->second.damaged || !ti->second.complete) { ++incomplete; continue; }
      ++rx_dropped;  // arrived complete but was dropped by a bounded receiver queue / decoder
    }
  }
  double dur = t_meas_first ? (r.t_end_seen ? (r.t_end_seen - t_meas_first) / 1e9 : (now_ns() - t_meas_first) / 1e9) : 0;
  double sender_dur = a.real("duration", 0);
  double fps_ok = sender_dur > 0 ? ok / sender_dur : (dur > 0 ? ok / dur : 0);
  Summary sf = summarize(l_first), sc = summarize(l_complete), sv = summarize(l_verified), sd = summarize(l_dec),
          ss = summarize(l_src_ver), su = summarize(unc), cp = summarize(cpu_p), cs = summarize(cpu_s);
  uint64_t failures = expected - ok;
  std::fprintf(stderr,
               "[receiver] SUMMARY expected=%llu ok=%llu corrupted=%llu incomplete=%llu never_arrived=%llu rx_dropped=%llu "
               "dup=%llu stamp_bad=%llu unknown=%llu | ok fps=%.2f | pkts=%llu lost=%llu | pay->complete med=%.2f p95=%.2f ms "
               "(sync ±%.3f) | decode med=%.2f p95=%.2f ms | cpu=%.0f%%(1core) sys=%.0f%%\n",
               (unsigned long long)expected, (unsigned long long)ok, (unsigned long long)corrupted,
               (unsigned long long)incomplete, (unsigned long long)never, (unsigned long long)rx_dropped,
               (unsigned long long)r.duplicates.load(), (unsigned long long)r.stamp_bad.load(),
               (unsigned long long)r.unknown.load(), fps_ok, (unsigned long long)r.packets.load(),
               (unsigned long long)r.lost.load(), sc.median, sc.p95, su.max, sd.median, sd.p95, cp.median, cs.median);
  if (!r.csv.empty()) {
    double rx_mbps = sender_dur > 0 && r.end_seen ? r.end_pkt.payload_bytes_measured * 8 / 1e6 / sender_dur : 0;
    csv_append(r.csv, {{"time", wall_clock_iso()}, {"label", r.label}, {"mode", r.mode}, {"rtp_path", r.rtp_path},
                       {"width", std::to_string(r.width)}, {"height", std::to_string(r.height)},
                       {"fps_nominal", std::to_string(r.fps)}, {"display", r.display},
                       {"end_seen", r.end_seen ? "1" : "0"},
                       {"sender_frames_measured", std::to_string(r.end_pkt.frames_measured)},
                       {"sender_queue_drops", std::to_string(r.end_pkt.sender_queue_drops)},
                       {"expected", std::to_string(expected)}, {"ok", std::to_string(ok)},
                       {"corrupted", std::to_string(corrupted)}, {"incomplete", std::to_string(incomplete)},
                       {"never_arrived", std::to_string(never)}, {"rx_dropped", std::to_string(rx_dropped)},
                       {"duplicates", std::to_string(r.duplicates.load())}, {"stamp_bad", std::to_string(r.stamp_bad.load())},
                       {"unknown", std::to_string(r.unknown.load())}, {"delivery_failures", std::to_string(failures)},
                       {"ok_fps", fmt(fps_ok, 2)}, {"packets", std::to_string(r.packets.load())},
                       {"packets_lost", std::to_string(r.lost.load())},
                       {"payload_mbps_sender", fmt(rx_mbps, 2)},
                       {"lat_pay_first_pkt_med_ms", fmt(sf.median)}, {"lat_pay_complete_med_ms", fmt(sc.median)},
                       {"lat_pay_complete_p95_ms", fmt(sc.p95)}, {"lat_pay_complete_max_ms", fmt(sc.max)},
                       {"lat_pay_verified_med_ms", fmt(sv.median)}, {"lat_src_verified_med_ms", fmt(ss.median)},
                       {"lat_src_verified_p95_ms", fmt(ss.p95)}, {"decode_ms_med", fmt(sd.median)},
                       {"decode_ms_p95", fmt(sd.p95)}, {"sync_unc_ms_max", fmt(su.max)},
                       {"display_frames", std::to_string(r.display_frames.load())},
                       {"cpu_proc_pct_one_core_med", fmt(cp.median, 1)}, {"cpu_sys_pct_med", fmt(cs.median, 1)},
                       {"exit_code", std::to_string(rc)}, {"args", a.dump()}});
  }
  if (bus) gst_object_unref(bus);
  if (dbus) gst_object_unref(dbus);
  if (r.disp_src) gst_object_unref(r.disp_src);
  if (r.decq) gst_object_unref(r.decq);
  gst_object_unref(pipe);
  if (dpipe) gst_object_unref(dpipe);
  return rc;
}
