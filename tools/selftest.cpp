// sb_selftest: checks the pieces both ends rely on.
//  1. XXH64 against published vectors and streaming vs one-shot equivalence.
//  2. Stamp write/read round trip at several resolutions.
//  3. Pattern determinism (same index -> same hash) and motion (different index -> different).
//  4. Canonical hash is format-independent (RGB vs BGRx of the same pixels).
// Prints "pattern <W>x<H> frame <n> hash" lines so Windows and Pi builds can be compared.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../src/common/canon.h"
#include "../src/common/pattern.h"
#include "../src/common/stamp.h"
#include "../src/common/xxhash64.h"

using namespace sb;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
  if (argc == 6 && std::strcmp(argv[1], "--dump") == 0) {  // --dump W H frame out.ppm
    int w = std::atoi(argv[2]), h = std::atoi(argv[3]);
    uint32_t id = static_cast<uint32_t>(std::atoll(argv[4]));
    std::vector<uint8_t> f(static_cast<size_t>(w) * 3 * h);
    PatternGen(w, h, 60, std::getenv("SB_SCENE") ? std::getenv("SB_SCENE") : "test").render(id, f.data(), w * 3);
    stamp_write(f.data(), w * 3, w, h, id);
    FILE* o = std::fopen(argv[5], "wb");
    if (!o) return 2;
    std::fprintf(o, "P6\n%d %d\n255\n", w, h);
    std::fwrite(f.data(), 1, f.size(), o);
    std::fclose(o);
    std::printf("wrote %s hash %016llx\n", argv[5], (unsigned long long)xxh64(f.data(), f.size()));
    return 0;
  }
  // 1. XXH64 vectors (seed 0)
  CHECK(xxh64("", 0) == 0xEF46DB3751D8E999ULL, "xxh64 empty = %016llx", (unsigned long long)xxh64("", 0));
  CHECK(xxh64("abc", 3) == 0x44BC2CF5AD770999ULL, "xxh64 abc = %016llx", (unsigned long long)xxh64("abc", 3));
  std::vector<uint8_t> buf(100003);
  for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(i * 31 + (i >> 7));
  uint64_t one = xxh64(buf.data(), buf.size());
  for (size_t chunk : {1u, 7u, 31u, 32u, 33u, 1000u, 2880u}) {
    Xxh64 h;
    for (size_t o = 0; o < buf.size(); o += chunk) h.update(buf.data() + o, std::min(chunk, buf.size() - o));
    CHECK(h.digest() == one, "streaming chunk %zu", chunk);
  }
  std::printf("xxh64(empty)=%016llx xxh64(abc)=%016llx xxh64(buf100003)=%016llx\n",
              (unsigned long long)xxh64("", 0), (unsigned long long)xxh64("abc", 3), (unsigned long long)one);

  // 2+3. stamp and pattern
  const int sizes[][2] = {{320, 180}, {960, 540}, {1280, 720}, {1920, 1080}, {3840, 2160}};
  for (auto& s : sizes) {
    int w = s[0], h = s[1], stride = w * 3;
    std::vector<uint8_t> f(static_cast<size_t>(stride) * h);
    PatternGen pg(w, h, 60);
    uint64_t prev = 0;
    for (uint32_t id : {0u, 1u, 59u, 60u, 123456u, 99999999u}) {
      pg.render(id, f.data(), stride);
      stamp_write(f.data(), stride, w, h, id);
      uint32_t got = 0;
      CHECK(stamp_read(f.data(), stride, w, h, &got) && got == id, "stamp %dx%d id %u got %u", w, h, id, got);
      uint64_t a = xxh64(f.data(), f.size());
      pg.render(id, f.data(), stride);
      stamp_write(f.data(), stride, w, h, id);
      CHECK(xxh64(f.data(), f.size()) == a, "pattern not deterministic %dx%d id %u", w, h, id);
      CHECK(a != prev, "pattern did not change %dx%d id %u", w, h, id);
      prev = a;
      if (id == 1u) std::printf("pattern %dx%d frame %u hash %016llx\n", w, h, id, (unsigned long long)a);
    }
    // 4. canonical hash RGB vs BGRx
    std::vector<uint8_t> bgrx(static_cast<size_t>(w) * 4 * h);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const uint8_t* p = &f[static_cast<size_t>(y) * stride + x * 3];
        uint8_t* q = &bgrx[(static_cast<size_t>(y) * w + x) * 4];
        q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = 0xAA;
      }
    uint64_t h1 = 0, h2 = 0;
    std::vector<uint8_t> scratch;
    canonical_rgb_hash_raw(f.data(), stride, w, h, GST_VIDEO_FORMAT_RGB, &h1, &scratch);
    canonical_rgb_hash_raw(bgrx.data(), w * 4, w, h, GST_VIDEO_FORMAT_BGRx, &h2, &scratch);
    CHECK(h1 == h2 && h1 == xxh64(f.data(), f.size()), "canonical hash mismatch %dx%d", w, h);
  }
  std::printf(fails ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", fails);
  return fails ? 1 : 0;
}
