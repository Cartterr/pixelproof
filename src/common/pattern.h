// Deterministic moving test pattern (packed RGB). Frame N always renders to the same bytes for
// a given size, so the receiver could even regenerate it; the benchmark compares hashes instead.
//
// Layout (below the frame-ID stamp band):
//   left  ~45%: scrolling fine text, 1-px glyph strokes in several colour pairs (incl. red/blue
//               and yellow/magenta, which 4:2:0 chroma subsampling smears)
//   right top : 1-px colour stripes, 1-px checkerboard, chroma zone (alternating R/B columns)
//   right bot : 256-step grey ramp and R/G/B ramps
//   whole frame: a bouncing high-contrast box (high motion) and a vertical sweep bar
#pragma once
#include <cstdint>
#include <vector>

namespace sb {

// Scenes:
//   test   - the layout above (fine text, colour detail, ramps, bouncing box, sweep) = high motion
//   text   - a full-frame document page (black/coloured 1-px text on white) scrolling 2 px/frame
//   static - the test layout frozen at frame 0 (only the frame-ID stamp changes)
class PatternGen {
 public:
  PatternGen(int width, int height, int fps, const char* scene = "test");
  // Renders frame `index` (motion phase) into dst (packed RGB, stride bytes per row).
  // The frame-ID stamp is drawn separately (stamp_write), on top of this.
  void render(uint32_t index, uint8_t* dst, int stride) const;
  int width() const { return w_; }
  int height() const { return h_; }

 private:
  int w_, h_, fps_;
  int scene_ = 0;  // 0 test, 1 text, 2 static
  std::vector<uint8_t> page_;  // scene "text": tall page strip, w*page_h_*3
  int page_h_ = 0;
  int text_x0_, text_y0_, text_w_, text_h_;
  std::vector<uint8_t> base_;   // static background with ramps/stripes, w*h*3
  std::vector<uint8_t> text_;   // tall pre-rendered text strip, text_w_ * text_strip_h_ * 3
  int text_strip_h_ = 0;
};

// Draws ASCII text with the built-in 8x12 font (scale >= 1) into packed RGB.
void draw_text(uint8_t* rgb, int stride, int w, int h, int x, int y, const char* s, int scale,
               uint8_t fr, uint8_t fg, uint8_t fb, bool opaque = false, uint8_t br = 0,
               uint8_t bg = 0, uint8_t bb = 0);

}  // namespace sb
