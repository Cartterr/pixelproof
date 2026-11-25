// Streaming XXH64 (reference algorithm by Yann Collet, re-implemented here so the
// sender and receiver share one dependency-free hash). Verified against the
// published test vectors in tools/selftest (see README).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sb {

class Xxh64 {
 public:
  explicit Xxh64(uint64_t seed = 0) { reset(seed); }

  void reset(uint64_t seed = 0) {
    seed_ = seed;
    v_[0] = seed + P1 + P2;
    v_[1] = seed + P2;
    v_[2] = seed;
    v_[3] = seed - P1;
    total_ = 0;
    buf_len_ = 0;
  }

  void update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    total_ += len;
    if (buf_len_ + len < 32) {
      std::memcpy(buf_ + buf_len_, p, len);
      buf_len_ += len;
      return;
    }
    if (buf_len_) {
      size_t fill = 32 - buf_len_;
      std::memcpy(buf_ + buf_len_, p, fill);
      p += fill;
      v_[0] = round(v_[0], rd64(buf_));
      v_[1] = round(v_[1], rd64(buf_ + 8));
      v_[2] = round(v_[2], rd64(buf_ + 16));
      v_[3] = round(v_[3], rd64(buf_ + 24));
      buf_len_ = 0;
    }
    uint64_t a = v_[0], b = v_[1], c = v_[2], d = v_[3];
    while (p + 32 <= end) {
      a = round(a, rd64(p));
      b = round(b, rd64(p + 8));
      c = round(c, rd64(p + 16));
      d = round(d, rd64(p + 24));
      p += 32;
    }
    v_[0] = a; v_[1] = b; v_[2] = c; v_[3] = d;
    if (p < end) {
      buf_len_ = static_cast<size_t>(end - p);
      std::memcpy(buf_, p, buf_len_);
    }
  }

  uint64_t digest() const {
    uint64_t h;
    if (total_ >= 32) {
      h = rotl(v_[0], 1) + rotl(v_[1], 7) + rotl(v_[2], 12) + rotl(v_[3], 18);
      h = merge(h, v_[0]);
      h = merge(h, v_[1]);
      h = merge(h, v_[2]);
      h = merge(h, v_[3]);
    } else {
      h = seed_ + P5;
    }
    h += total_;
    const uint8_t* p = buf_;
    const uint8_t* end = buf_ + buf_len_;
    while (p + 8 <= end) {
      h ^= round(0, rd64(p));
      h = rotl(h, 27) * P1 + P4;
      p += 8;
    }
    if (p + 4 <= end) {
      h ^= static_cast<uint64_t>(rd32(p)) * P1;
      h = rotl(h, 23) * P2 + P3;
      p += 4;
    }
    while (p < end) {
      h ^= (*p) * P5;
      h = rotl(h, 11) * P1;
      ++p;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
  }

 private:
  static constexpr uint64_t P1 = 0x9E3779B185EBCA87ULL;
  static constexpr uint64_t P2 = 0xC2B2AE3D27D4EB4FULL;
  static constexpr uint64_t P3 = 0x165667B19E3779F9ULL;
  static constexpr uint64_t P4 = 0x85EBCA77C2B2AE63ULL;
  static constexpr uint64_t P5 = 0x27D4EB2F165667C5ULL;

  static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
  static uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }  // little-endian hosts only
  static uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
  static uint64_t round(uint64_t acc, uint64_t in) {
    acc += in * P2;
    acc = rotl(acc, 31);
    return acc * P1;
  }
  static uint64_t merge(uint64_t acc, uint64_t v) {
    acc ^= round(0, v);
    return acc * P1 + P4;
  }

  uint64_t seed_ = 0;
  uint64_t v_[4] = {};
  uint64_t total_ = 0;
  uint8_t buf_[32] = {};
  size_t buf_len_ = 0;
};

inline uint64_t xxh64(const void* p, size_t n, uint64_t seed = 0) {
  Xxh64 h(seed);
  h.update(p, n);
  return h.digest();
}

}  // namespace sb
