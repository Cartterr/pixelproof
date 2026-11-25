#include "stamp.h"

#include <algorithm>
#include <cstring>

namespace sb {
namespace {

inline void fill_rect(uint8_t* rgb, int stride, int w, int h, int x0, int y0, int rw, int rh,
                      uint8_t r, uint8_t g, uint8_t b) {
  int x1 = std::min(w, x0 + rw), y1 = std::min(h, y0 + rh);
  x0 = std::max(0, x0);
  y0 = std::max(0, y0);
  for (int y = y0; y < y1; ++y) {
    uint8_t* p = rgb + static_cast<size_t>(y) * stride + static_cast<size_t>(x0) * 3;
    for (int x = x0; x < x1; ++x, p += 3) {
      p[0] = r; p[1] = g; p[2] = b;
    }
  }
}

// Segment bit order: a b c d e f g (top, top-right, bottom-right, bottom, bottom-left, top-left, middle)
const uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

void draw_digit(uint8_t* rgb, int stride, int w, int h, int x, int y, int dw, int dh, int d) {
  int t = std::max(1, dh / 9);
  int half = dh / 2;
  uint8_t s = kSeg[d % 10];
  auto seg = [&](int bit, int sx, int sy, int sw, int sh) {
    if (s & (1 << bit)) fill_rect(rgb, stride, w, h, x + sx, y + sy, sw, sh, 255, 255, 255);
  };
  seg(0, t, 0, dw - 2 * t, t);                 // a
  seg(1, dw - t, t, t, half - t);              // b
  seg(2, dw - t, half, t, dh - half - t);      // c
  seg(3, t, dh - t, dw - 2 * t, t);            // d
  seg(4, 0, half, t, dh - half - t);           // e
  seg(5, 0, t, t, half - t);                   // f
  seg(6, t, half - t / 2, dw - 2 * t, t);      // g
}

}  // namespace

StampLayout stamp_layout(int width, int height) {
  StampLayout L;
  L.cell = std::clamp(width / 128, 2, 8);
  L.digit_h = std::clamp(height / 8, 10, 200);
  L.digit_w = std::max(6, L.digit_h * 55 / 100);
  int gap = std::max(2, L.digit_h / 8);
  int digits_w = 8 * (L.digit_w + gap);
  int avail = width - digits_w - 3 * gap;
  L.gray_block = std::clamp(std::min(L.digit_h / 2, avail / 18), 2, 200);
  L.band_w = std::min(width, std::max(64 * L.cell, digits_w + gap + 18 * L.gray_block + gap));
  L.band_h = std::min(height, L.cell + gap + L.digit_h + gap);
  return L;
}

int stamp_code_rows(int width, int height) { return stamp_layout(width, height).cell; }

void stamp_write(uint8_t* rgb, int stride, int w, int h, uint32_t id) {
  StampLayout L = stamp_layout(w, h);
  int gap = std::max(2, L.digit_h / 8);
  fill_rect(rgb, stride, w, h, 0, 0, L.band_w, L.band_h, 0, 0, 0);

  // Machine code: id then ~id, MSB first.
  uint64_t word = (static_cast<uint64_t>(id) << 32) | static_cast<uint32_t>(~id);
  for (int i = 0; i < 64; ++i) {
    bool bit = (word >> (63 - i)) & 1;
    uint8_t v = bit ? 255 : 0;
    fill_rect(rgb, stride, w, h, i * L.cell, 0, L.cell, L.cell, v, v, v);
  }

  // Eight decimal digits.
  int y = L.cell + gap;
  uint32_t n = id;
  for (int i = 7; i >= 0; --i) {
    draw_digit(rgb, stride, w, h, i * (L.digit_w + gap) + gap / 2, y, L.digit_w, L.digit_h, n % 10);
    n /= 10;
  }

  // Reference white/black blocks then 16 Gray-code bits (MSB first).
  int gx = 8 * (L.digit_w + gap) + gap;
  int gy = y + (L.digit_h - L.gray_block) / 2;
  fill_rect(rgb, stride, w, h, gx, gy, L.gray_block, L.gray_block, 255, 255, 255);
  gx += 2 * L.gray_block;  // leave the black reference block (already black)
  uint32_t gray = (id ^ (id >> 1)) & 0xFFFF;
  for (int i = 0; i < 16; ++i) {
    bool bit = (gray >> (15 - i)) & 1;
    if (bit) fill_rect(rgb, stride, w, h, gx + i * L.gray_block, gy, L.gray_block, L.gray_block, 255, 255, 255);
  }
}

bool stamp_read(const uint8_t* rgb, int stride, int w, int h, uint32_t* frame_id, int bpp) {
  StampLayout L = stamp_layout(w, h);
  if (64 * L.cell > w || L.cell > h) return false;
  uint64_t word = 0;
  int cy = L.cell / 2;
  for (int i = 0; i < 64; ++i) {
    int cx = i * L.cell + L.cell / 2;
    const uint8_t* p = rgb + static_cast<size_t>(cy) * stride + static_cast<size_t>(cx) * bpp;
    int lum = p[0] + p[1] + p[2];
    word = (word << 1) | (lum > 382 ? 1u : 0u);
  }
  uint32_t hi = static_cast<uint32_t>(word >> 32), lo = static_cast<uint32_t>(word);
  if (hi != static_cast<uint32_t>(~lo)) return false;
  *frame_id = hi;
  return true;
}

}  // namespace sb
