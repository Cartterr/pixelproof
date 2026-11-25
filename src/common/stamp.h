// Frame-ID stamp drawn into the top-left corner of every transmitted frame.
//
//  row 0 (cell px tall): 64 black/white cells = frame_id (32 bits) then ~frame_id (32 bits).
//                        Exact for lossless paths; the receiver reads it back to pair frames.
//  below it:             8 large 7-segment digits (human / camera readable) and a row of
//                        16 Gray-code blocks plus a white and a black reference block, so a
//                        phone video of the TV can be decoded frame by frame.
// Everything is drawn in packed RGB (3 bytes per pixel, any row stride).
#pragma once
#include <cstdint>

namespace sb {

struct StampLayout {
  int cell = 2;        // machine-code cell size (px)
  int digit_h = 10;    // 7-segment digit height
  int digit_w = 6;
  int gray_block = 6;  // Gray-code block size
  int band_w = 0;      // stamped area (always starts at 0,0)
  int band_h = 0;
};

StampLayout stamp_layout(int width, int height);

// Draws the stamp. rgb points at pixel (0,0); stride in bytes.
void stamp_write(uint8_t* rgb, int stride, int width, int height, uint32_t frame_id);

// Reads the machine code. Returns false if the cells are not a valid id/~id pair.
// bpp = bytes per pixel (3 for RGB/BGR, 4 for RGBx/BGRx; cells are black/white so order is moot).
bool stamp_read(const uint8_t* rgb, int stride, int width, int height, uint32_t* frame_id, int bpp = 3);

// Number of image rows that must be present to read the machine code.
int stamp_code_rows(int width, int height);

}  // namespace sb
