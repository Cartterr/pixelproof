#!/bin/bash
# Build sb_receiver / sb_sender / sb_selftest on Raspberry Pi OS (Debian 13 trixie, arm64).
# Installs the build dependencies first (idempotent). Run from the stream-bench folder on the Pi.
set -euo pipefail
cd "$(dirname "$0")/.."
if ! pkg-config --exists gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtp-1.0 2>/dev/null; then
  sudo apt-get update
  sudo apt-get install -y build-essential cmake pkg-config gstreamer1.0-tools libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-libav iperf3 ethtool
fi
cmake -S . -B build/pi -DCMAKE_BUILD_TYPE=Release
cmake --build build/pi -j"$(nproc)"
echo "Built: $(pwd)/build/pi/sb_receiver $(pwd)/build/pi/sb_sender $(pwd)/build/pi/sb_selftest"
