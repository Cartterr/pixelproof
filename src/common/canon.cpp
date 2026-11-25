#include "canon.h"

#include <cstring>

#include "xxhash64.h"

namespace sb {

bool canonical_rgb_hash_raw(const uint8_t* data, int stride, int w, int h, GstVideoFormat fmt,
                            uint64_t* out, std::vector<uint8_t>* scratch) {
  int bpp, ri, gi, bi;
  switch (fmt) {
    case GST_VIDEO_FORMAT_RGB: bpp = 3; ri = 0; gi = 1; bi = 2; break;
    case GST_VIDEO_FORMAT_BGR: bpp = 3; ri = 2; gi = 1; bi = 0; break;
    case GST_VIDEO_FORMAT_RGBx: case GST_VIDEO_FORMAT_RGBA: bpp = 4; ri = 0; gi = 1; bi = 2; break;
    case GST_VIDEO_FORMAT_BGRx: case GST_VIDEO_FORMAT_BGRA: bpp = 4; ri = 2; gi = 1; bi = 0; break;
    case GST_VIDEO_FORMAT_xRGB: case GST_VIDEO_FORMAT_ARGB: bpp = 4; ri = 1; gi = 2; bi = 3; break;
    case GST_VIDEO_FORMAT_xBGR: case GST_VIDEO_FORMAT_ABGR: bpp = 4; ri = 3; gi = 2; bi = 1; break;
    default: return false;
  }
  Xxh64 hh;
  const size_t row = static_cast<size_t>(w) * 3;
  if (bpp == 3 && ri == 0) {
    for (int y = 0; y < h; ++y) hh.update(data + static_cast<size_t>(y) * stride, row);
  } else {
    if (scratch->size() < row) scratch->resize(row);
    uint8_t* o = scratch->data();
    for (int y = 0; y < h; ++y) {
      const uint8_t* p = data + static_cast<size_t>(y) * stride;
      for (int x = 0; x < w; ++x, p += bpp) {
        o[3 * x] = p[ri];
        o[3 * x + 1] = p[gi];
        o[3 * x + 2] = p[bi];
      }
      hh.update(o, row);
    }
  }
  *out = hh.digest();
  return true;
}

bool canonical_rgb_hash(const GstVideoFrame* f, uint64_t* out, std::vector<uint8_t>* scratch) {
  return canonical_rgb_hash_raw(static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(f, 0)),
                                GST_VIDEO_FRAME_PLANE_STRIDE(f, 0), GST_VIDEO_FRAME_WIDTH(f),
                                GST_VIDEO_FRAME_HEIGHT(f), GST_VIDEO_FRAME_FORMAT(f), out, scratch);
}

}  // namespace sb
