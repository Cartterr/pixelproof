// Side-channel messages between sender and receiver (UDP, separate port from the video).
//
//  * FRAME records: for each frame that reached the payloader, its id, RTP timestamp, XXH64 of
//    the canonical RGB bytes, and the sender's timestamp converted into the receiver's clock via
//    the running clock-offset estimate (with its uncertainty). Each packet repeats the last 4
//    records so a lost side packet does not lose a record.
//  * SYNC ping/pong: NTP-style offset estimation. The SENDER initiates (so Windows Firewall
//    only sees replies to an outbound flow); the receiver answers with its monotonic time.
//  * END: sender totals; the receiver finalises its accounting when it sees this.
#pragma once
#include <cstdint>

namespace sb {

constexpr uint32_t kMagicFrame = 0x31534253;  // "SBS1"
constexpr uint32_t kMagicPing = 0x31544253;   // "SBT1"
constexpr uint32_t kMagicEnd = 0x31454253;    // "SBE1"

enum RecordFlags : uint32_t {
  kFlagWarmup = 1u << 0,  // frame was sent during warm-up; excluded from statistics
};

#pragma pack(push, 1)
struct FrameRecord {
  uint32_t frame_id;
  uint32_t rtp_ts;
  uint32_t flags;
  uint32_t payload_bytes;   // bytes handed to the payloader (raw frame or compressed frame)
  uint64_t hash;            // XXH64 of canonical packed RGB (R,G,B per pixel, no padding)
  int64_t t_pay_s;          // sender clock, frame entered the payloader
  int64_t t_pay_r;          // same instant in receiver clock (0 if no sync yet)
  int64_t t_src_s;          // sender clock, frame was stamped (after capture/convert)
  uint32_t sync_unc_ns;     // +/- uncertainty of t_pay_r (half the best RTT + drift allowance)
  uint32_t reserved;
};

struct FramePacket {
  uint32_t magic;
  uint32_t count;
  FrameRecord rec[4];
};

struct PingPacket {
  uint32_t magic;
  uint32_t seq;
  int64_t t1_s;  // sender clock when sent
  int64_t t2_r;  // receiver clock when answered (0 in the request)
};

struct EndPacket {
  uint32_t magic;
  uint32_t last_id;
  uint32_t frames_measured;   // frames sent outside warm-up
  uint32_t sender_queue_drops;
  uint64_t payload_bytes_measured;
};
#pragma pack(pop)

}  // namespace sb
