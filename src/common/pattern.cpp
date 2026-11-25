#include "pattern.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "font_8x12.h"
#include "stamp.h"

namespace sb {
namespace {

inline void put(uint8_t* rgb, int stride, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t* p = rgb + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
  p[0] = r; p[1] = g; p[2] = b;
}

void rect(uint8_t* rgb, int stride, int w, int h, int x0, int y0, int rw, int rh, uint8_t r,
          uint8_t g, uint8_t b) {
  int x1 = std::min(w, x0 + rw), y1 = std::min(h, y0 + rh);
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) put(rgb, stride, x, y, r, g, b);
}

// Integer hue wheel 0..1535 -> RGB.
void hue(int hh, uint8_t& r, uint8_t& g, uint8_t& b) {
  hh %= 1536;
  int s = hh / 256, f = hh % 256;
  switch (s) {
    case 0: r = 255; g = f; b = 0; break;
    case 1: r = 255 - f; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = f; break;
    case 3: r = 0; g = 255 - f; b = 255; break;
    case 4: r = f; g = 0; b = 255; break;
    default: r = 255; g = 0; b = 255 - f; break;
  }
}

struct ColorPair { uint8_t f[3], b[3]; };
const ColorPair kPairs[] = {
    {{255, 255, 255}, {0, 0, 0}},     {{255, 0, 0}, {0, 0, 255}},   {{255, 255, 0}, {255, 0, 255}},
    {{0, 255, 0}, {255, 0, 0}},       {{0, 0, 0}, {255, 255, 255}}, {{0, 255, 255}, {128, 0, 0}},
    {{255, 128, 0}, {0, 64, 128}},    {{200, 200, 200}, {40, 40, 40}},
};

}  // namespace

void draw_text(uint8_t* rgb, int stride, int w, int h, int x, int y, const char* s, int scale,
               uint8_t fr, uint8_t fg, uint8_t fb, bool opaque, uint8_t br, uint8_t bg, uint8_t bb) {
  for (; *s; ++s, x += kFontW * scale) {
    int c = static_cast<unsigned char>(*s);
    if (c < 32 || c > 126) c = '?';
    const uint8_t* gl = kFont8x12[c - 32];
    for (int gy = 0; gy < kFontH; ++gy)
      for (int gx = 0; gx < kFontW; ++gx) {
        bool on = gl[gy] & (0x80 >> gx);
        if (!on && !opaque) continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx) {
            int px = x + gx * scale + sx, py = y + gy * scale + sy;
            if (px < 0 || py < 0 || px >= w || py >= h) continue;
            if (on) put(rgb, stride, px, py, fr, fg, fb);
            else put(rgb, stride, px, py, br, bg, bb);
          }
      }
  }
}

PatternGen::PatternGen(int width, int height, int fps, const char* scene)
    : w_(width), h_(height), fps_(fps) {
  std::string sc = scene ? scene : "test";
  scene_ = sc == "text" ? 1 : sc == "static" ? 2 : 0;
  if (scene_ == 1) {
    // Document page: white background, mostly black text, some coloured words and links,
    // 2x as tall as the frame plus one screen so the scroll wraps seamlessly.
    page_h_ = ((2 * h_) / kFontH + 1) * kFontH;
    const int pstride = w_ * 3;
    page_.assign(static_cast<size_t>(pstride) * page_h_, 255);
    static const char* words[] = {"stream", "latency", "pixel", "frame", "raspberry", "gstreamer", "the",
                                  "of", "and", "RGB", "lossless", "FFV1", "HEVC", "display", "network",
                                  "0123456789", "packet", "buffer", "decode", "encode", "a", "to", "is"};
    const int nwords = sizeof(words) / sizeof(words[0]);
    uint32_t rng = 12345;
    char line[1024];
    for (int row = 0; row * kFontH < page_h_; ++row) {
      int cols = (w_ - 16) / kFontW;
      std::snprintf(line, sizeof line, "%5d  ", row);
      std::string s = line;
      while (static_cast<int>(s.size()) < cols - 12) {
        rng = rng * 1103515245u + 12345u;
        s += words[(rng >> 16) % nwords];
        s += ' ';
      }
      s.resize(std::min<size_t>(s.size(), cols));
      rng = rng * 1103515245u + 12345u;
      int kind = (rng >> 16) % 10;
      uint8_t r = 0, g = 0, b = 0;
      if (kind == 7) { b = 200; }                 // link blue
      else if (kind == 8) { r = 200; }            // red
      else if (kind == 9) { g = 120; b = 0; }     // green
      if (row % 25 == 0) draw_text(page_.data(), pstride, w_, page_h_, 8, row * kFontH, s.c_str(), 1, 255, 255, 255, true, 40, 40, 120);
      else draw_text(page_.data(), pstride, w_, page_h_, 8, row * kFontH, s.c_str(), 1, r, g, b);
    }
  }
  const int stride = w_ * 3;
  base_.assign(static_cast<size_t>(stride) * h_, 0);
  uint8_t* B = base_.data();

  // Background: hue across, brightness down.
  for (int y = 0; y < h_; ++y) {
    int lum = 64 + (y * 160) / std::max(1, h_ - 1);
    for (int x = 0; x < w_; ++x) {
      uint8_t r, g, b;
      hue((x * 1536) / std::max(1, w_), r, g, b);
      put(B, stride, x, y, static_cast<uint8_t>(r * lum / 255), static_cast<uint8_t>(g * lum / 255),
          static_cast<uint8_t>(b * lum / 255));
    }
  }

  StampLayout L = stamp_layout(w_, h_);
  int top = L.band_h + 2;
  text_x0_ = 4;
  text_y0_ = top;
  text_w_ = std::max(8, (w_ * 45) / 100 - 8);
  text_h_ = std::max(kFontH, h_ - top - std::max(8, h_ / 10) - 4);

  // Right-top: detail tiles.
  int rx = (w_ * 47) / 100, rw = w_ - rx - 4;
  int ry = top, rh = std::max(4, (h_ - top) / 2 - 4);
  int tile_w = std::max(4, rw / 3);
  for (int y = ry; y < ry + rh && y < h_; ++y) {
    for (int x = rx; x < rx + rw && x < w_; ++x) {
      int lx = x - rx;
      uint8_t r = 0, g = 0, b = 0;
      if (lx < tile_w) {  // 1-px colour stripes R G B C M Y W K
        static const uint8_t c[8][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 255, 255},
                                        {255, 0, 255}, {255, 255, 0}, {255, 255, 255}, {0, 0, 0}};
        const uint8_t* q = c[lx % 8];
        r = q[0]; g = q[1]; b = q[2];
      } else if (lx < 2 * tile_w) {  // 1-px checkerboard
        uint8_t v = ((x + y) & 1) ? 255 : 0;
        r = g = b = v;
      } else {  // chroma zone: red/blue columns, rows alternate with green/magenta
        if (y & 1) { if (x & 1) { r = 255; } else { b = 255; } }
        else { if (x & 1) { g = 255; } else { r = 255; b = 255; } }
      }
      put(B, stride, x, y, r, g, b);
    }
  }
  // Right-bottom: ramps.
  int qy = ry + rh + 4, qh = std::max(4, h_ - qy - 4);
  int band = std::max(1, qh / 4);
  for (int y = qy; y < qy + qh && y < h_; ++y) {
    int k = (y - qy) / band;
    for (int x = rx; x < rx + rw && x < w_; ++x) {
      uint8_t v = static_cast<uint8_t>(((x - rx) * 256) / std::max(1, rw));
      uint8_t r = 0, g = 0, b = 0;
      if (k == 0) r = g = b = v;
      else if (k == 1) r = v;
      else if (k == 2) g = v;
      else b = v;
      put(B, stride, x, y, r, g, b);
    }
  }

  // Tall text strip (3x the visible height) that scrolls 2 px per frame.
  text_strip_h_ = ((3 * text_h_) / kFontH + 1) * kFontH;
  text_.assign(static_cast<size_t>(text_w_) * 3 * text_strip_h_, 0);
  const int tstride = text_w_ * 3;
  char line[512];
  for (int row = 0; row * kFontH < text_strip_h_; ++row) {
    const ColorPair& cp = kPairs[row % (sizeof(kPairs) / sizeof(kPairs[0]))];
    rect(text_.data(), tstride, text_w_, text_strip_h_, 0, row * kFontH, text_w_, kFontH, cp.b[0],
         cp.b[1], cp.b[2]);
    std::snprintf(line, sizeof line,
                  "L%04d The quick brown fox jumps over 13 lazy dogs. 0123456789 "
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz {}[]()<>/\\|@#$%%&*+=-_~",
                  row);
    draw_text(text_.data(), tstride, text_w_, text_strip_h_, 2, row * kFontH, line, 1, cp.f[0],
              cp.f[1], cp.f[2]);
  }
}

void PatternGen::render(uint32_t index, uint8_t* dst, int stride) const {
  const int bstride = w_ * 3;
  if (scene_ == 1) {  // scrolling document page, 2 px per frame
    int off = static_cast<int>((static_cast<uint64_t>(index) * 2) % static_cast<uint64_t>(page_h_));
    for (int y = 0; y < h_; ++y)
      std::memcpy(dst + static_cast<size_t>(y) * stride,
                  page_.data() + static_cast<size_t>((off + y) % page_h_) * bstride, bstride);
    return;
  }
  if (scene_ == 2) index = 0;  // static scene: motion frozen
  for (int y = 0; y < h_; ++y)
    std::memcpy(dst + static_cast<size_t>(y) * stride, base_.data() + static_cast<size_t>(y) * bstride, bstride);

  // Scrolling text window.
  const int tstride = text_w_ * 3;
  int off = static_cast<int>((static_cast<uint64_t>(index) * 2) % static_cast<uint64_t>(text_strip_h_));
  for (int y = 0; y < text_h_ && text_y0_ + y < h_; ++y) {
    int sy = (off + y) % text_strip_h_;
    std::memcpy(dst + static_cast<size_t>(text_y0_ + y) * stride + static_cast<size_t>(text_x0_) * 3,
                text_.data() + static_cast<size_t>(sy) * tstride, std::min(tstride, (w_ - text_x0_) * 3));
  }

  // Vertical sweep bar: one full sweep per second.
  int sweep_w = std::max(2, w_ / 160);
  int sx = static_cast<int>((static_cast<uint64_t>(index % fps_) * (w_ - sweep_w)) / std::max(1, fps_ - 1));
  StampLayout L = stamp_layout(w_, h_);
  rect(dst, stride, w_, h_, sx, L.band_h, sweep_w, h_ - L.band_h, 255, 255, 255);

  // Bouncing box: crosses the frame in ~1 s (high motion), colour cycles every 8 frames.
  int bs = std::max(8, h_ / 6);
  int span = std::max(1, w_ - bs);
  int period = 2 * span;
  int step = std::max(1, (2 * span) / std::max(1, fps_));
  int pos = static_cast<int>((static_cast<uint64_t>(index) * step) % period);
  int bx = pos < span ? pos : period - pos;
  int by = L.band_h + (h_ - L.band_h - bs) / 2 + static_cast<int>((index % 32) < 16 ? (index % 16) : 16 - (index % 16)) * std::max(1, h_ / 200);
  uint8_t r, g, b;
  hue(static_cast<int>((index / 8) * 192), r, g, b);
  rect(dst, stride, w_, h_, bx, by, bs, bs, r, g, b);
  // Inner 2-px checker so motion blur / scaling is visible.
  for (int y = by + bs / 4; y < by + 3 * bs / 4 && y < h_; ++y)
    for (int x = bx + bs / 4; x < bx + 3 * bs / 4 && x < w_; ++x)
      if (((x - bx) / 2 + (y - by) / 2) & 1) put(dst, stride, x, y, 0, 0, 0);

  // Frame index in plain text inside the box (fine detail on a moving target).
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u", index);
  draw_text(dst, stride, w_, h_, bx + 2, by + 2, buf, 1, 255, 255, 255, true, 0, 0, 0);
}

}  // namespace sb
