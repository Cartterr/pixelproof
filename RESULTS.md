# Results — 2026-11-24

Windows laptop → Raspberry Pi 5, direct Ethernet cable. Every value here is **observed**
(from `results/summary.csv` and the per-sample folders) unless it is explicitly marked as an
estimate. Each mode is 3 samples × 30 s after a 5 s warm-up unless noted.

## Setup

| | |
|---|---|
| Sender | Alienware 16 Area-51, Core Ultra 9 275HX (24 cores), RTX 5080 Laptop + Intel iGPU, Windows 11 26200, GStreamer **1.28.9** (official MSVC x86_64 build), MSVC 14.44 |
| Receiver | Raspberry Pi 5 Rev 1.1, 16 GB, Raspberry Pi OS (Debian 13 trixie), kernel 6.18, arm64, GStreamer **1.26.2** (distribution packages), labwc Wayland session |
| Pi power / cooling | Raspberry Pi 5 A USB-C supply (PD reports 5000 mA, EXT5V ≈ 5.10–5.16 V), active cooler (fan ≈ 3,800 rpm), governor `performance`, ARM 2.4 GHz, booted from a USB flash drive |
| Pi thermals | 55–66 °C across all runs, `get_throttled = 0x0` at the start and end of every sample |
| Link (passes A, B) | Realtek RTL8156 USB 2.5GbE adapter on the laptop ↔ Pi built-in GbE, **1000 Mb/s full duplex**, temporary 10.77.0.1/.2, no gateway; management stayed on Wi-Fi/Tailscale |
| Link check | ping 0.37 ms both ways; iperf3 TCP 949 Mb/s (laptop→Pi) / 944 Mb/s (Pi→laptop); UDP 950 Mb/s offered → 948 Mb/s, 0.13 % loss |
| Display | none attached yet (Pi HDMI ports report `disconnected`), so every result below is a **receiver-side** measurement; frames were verified, not shown on a TV |

## Pass A — raw RGB (RTP RFC 4175), 960×540 @ 60, 8-bit RGB

Payload ≈ 746 Mb/s, ≈ 795 Mb/s on the wire. 1280×720 @ 30 fallback was not needed.

| Source | Sent fps | Verified bit-exact | Corrupted / incomplete / never arrived / Pi drops | Packets lost | Payloader → last packet (median / p95) | Frame ready → verified on Pi (median / p95) | Wire rate | Pi CPU (receiver process) |
|---|---|---|---|---|---|---|---|---|
| Generated pattern | 60.03–60.10 | **5406 / 5406** | 0 / 0 / 0 / 0 | 0 | 13.5 / 14.8–18.3 ms | 14.3 / 15.6–19.1 ms | 795 Mb/s | 58–78 % of one core |
| Desktop (moving pattern on the panel, Desktop Duplication) | 51.6–53.0 | **4698 / 4698** | 0 / 0 / 0 / 0 | 0 | 13.5 / 15.2–15.5 ms | 14.2 / 15.9–16.4 ms | 683–701 Mb/s | 47–56 % |
| Desktop (idle) | 22.8–34.6 | **2753 / 2753** | 0 / 0 / 0 / 0 | 0 | 13.6 / 14.6–15.1 ms | 14.3 / 15.3–16.0 ms | 301–459 Mb/s | 22–35 % |

* The desktop rows send fewer than 60 frames/s because **Desktop Duplication only delivers a frame
  when the screen changes** (49–57/s with a 60 fps scene, 23–35/s idle). That is the capture side;
  every frame it produced arrived intact. Sender queue drops: 1 in one idle sample, otherwise 0.
* The ~13.5 ms is almost exactly the time to serialise one 1.55 MB frame at gigabit line rate
  (≈ 13 ms): **the link is the bottleneck, and the latency is the wire.**
* Clock-sync uncertainty for every latency figure: < ±0.8 ms (sender-initiated pings, min-RTT
  per second, drift fit). NIC drops, UDP receive-buffer errors: 0 in every sample.

## Pass B — FFV1 lossless (`avenc_ffv1` BGRx/RCT, intra-only, 16 slices, `rtpgstpay`), 1920×1080 @ 30

| Source | Sent fps | Verified bit-exact after decode | Failures | Packets lost | FFV1 bitrate | Encode (laptop, median / p95) | Decode (Pi, median / p95) | Payloader → last packet | Frame ready → verified (median / p95) | Pi CPU |
|---|---|---|---|---|---|---|---|---|---|---|
| Generated pattern | 30.00–30.03 | **2702 / 2702** | 0 | 0 | 221.6 Mb/s (6.7 : 1) | 6.3 / 7.0 ms | 16.3 / 23.6–26.7 ms | 10.8 ms | 39.2 / 50–57 ms | ≈ 196 % (2 cores) |
| Desktop, idle | 27.8–30.0 | **2582 / 2582** | 0 | 0 | 85–108 Mb/s (≈ 15 : 1) | 5.6–7.3 / 6.4–9.0 ms | 12.0–18.1 / 16.7–25.4 ms | 5.0 ms | 31.6 / 32–54 ms | 135–183 % |
| Desktop, scrolling text page | 30.03 | **2703 / 2703** | 0 | 0 | 287.6 Mb/s (5.2 : 1) | 8.6 / 10.2–10.5 ms | 22.9 / 34–38 ms | 16.3 ms | 54.1 / 85–109 ms | ≈ 250 % |
| Desktop, high-motion pattern | — | — | — | — | — | — | — | — | — | — |

* Raw 1080p30 RGB would be 1.49 Gb/s; FFV1 brings it to 85–288 Mb/s, so the link has plenty of headroom.
* **The Pi's software FFV1 decoder is the limit**: 16–23 ms median per 1080p frame with 4 slice
  threads, p95 up to 38 ms. That already exceeds the 16.7 ms budget of 60 fps, so FFV1 1080p60 would not keep up on the scrolling-text content.
  This is an **estimate** from the 30 fps decode times; 1080p60 was not run.
* The high-motion desktop row and all 1080p60 rows are missing: the laptop hard-reset during the
  first sample of that mode (see *Incident*).

## 100 Mb/s link (before the gigabit adapter)

Realtek RTL8152 USB Fast Ethernet: iperf3 94.8 Mb/s TCP both ways, 95 Mb/s UDP with 0 loss.

| Mode | Result |
|---|---|
| Raw 320×180@30 pattern / desktop / Wayland display | 121/121, 85/85, 121/121 frames bit-exact, 15.3 ms payloader → last packet |
| FFV1 640×360@30 pattern | 121/121 bit-exact, 28 Mb/s (5.9 : 1), encode 0.9 ms, decode 2.2 ms |
| Raw 960×540@60 (Pass A on 100 Mb/s) | sender stays responsive by dropping ≈ 53/60 frames in its bounded queue; 7.6 fps delivered, all bit-exact, 0 packets lost; 308 ms payloader → last packet (176 ms of it waiting in the 2 MiB socket buffer) |

## What this answers so far

1. **Fastest pixel-perfect mode sustained:** raw RGB 960×540 at 60 fps (≈ 746 Mb/s payload),
   5406/5406 frames bit-exact, 14.3 ms median from "frame ready on the laptop" to "verified on the
   Pi". Lossless 1080p30 also holds (FFV1, 7,987 frames bit-exact) at 32–54 ms.
2. **4K60 HEVC (Sunshine → Moonlight):** not run yet. Moonlight 6.2.0 is installed on the Pi,
   Sunshine runs on the laptop, and a genuine 3840×2160@60 virtual monitor was demonstrated; it
   needs the TV on the Pi's HDMI and a pairing PIN.
3. **PyroWave:** no meaningful comparison possible on this hardware. Valve states the Pi's GPU
   cannot decode PyroWave; Steam Link for Pi does HEVC 4K60 but not PyroWave. A supported receiver
   would be Steam on Windows/macOS/Linux or the Steam Link app on Apple/Android.
4. **What sets the limit:** for raw RGB the **gigabit Ethernet link** (13.5 of 14.3 ms is wire time;
   960×540@60 is the largest 60 fps raw mode that fits). For lossless, the **Pi's CPU FFV1 decode**.
   Single most useful change for pixel-exact video: a faster lossless codec path on the Pi (or a
   2.5 GbE path, which the Pi 5's built-in port cannot do).

## Incident: laptop hard reset

At 20:48:29 the laptop reset without a bug check (Kernel-Power 41, `BugcheckCode = 0`, no
minidump). The firmware reported a WHEA boot error and Windows logged corrected PCIe AER errors on
the RTX 5080 and the Wi-Fi 7 card on the next boot; a GPU watchdog live-kernel report had been
written at 20:15. It happened while the heaviest graphics combination of the run was active: a
full-screen 2560×1600 @ 60 preview on the internal panel, Desktop Duplication of that same panel,
and 16-slice FFV1 encoding. The same combination had run for ~7 minutes earlier without trouble,
so the root cause is not proven; treat that combination as unsafe on this machine.
