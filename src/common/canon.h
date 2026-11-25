// Canonical RGB hashing: XXH64 over R,G,B bytes per pixel, rows concatenated with no padding,
// independent of the in-memory format (RGB, BGR, RGBx, BGRx, xRGB, xBGR, RGBA, BGRA) and stride.
// Sender and receiver both use this, so channel order and row padding are handled identically.
#pragma once
#include <gst/video/video.h>

#include <cstdint>
#include <vector>

namespace sb {

// Returns false if the format is not a supported 8-bit RGB layout.
bool canonical_rgb_hash(const GstVideoFrame* frame, uint64_t* out, std::vector<uint8_t>* scratch);
bool canonical_rgb_hash_raw(const uint8_t* data, int stride, int width, int height,
                            GstVideoFormat fmt, uint64_t* out, std::vector<uint8_t>* scratch);

}  // namespace sb
