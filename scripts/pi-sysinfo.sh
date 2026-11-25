#!/bin/bash
# One-shot Pi state snapshot (key=value), recorded at the start and end of every benchmark sample.
# Covers thermals/throttling/governor/power plus NIC and UDP drop counters, so packet loss can be
# attributed to the wire/NIC (eth0 drops) or to the receiving socket (UDP RcvbufErrors).
export LC_ALL=C
echo "time=$(date -Is)"
echo "temp_c=$(vcgencmd measure_temp | grep -o '[0-9.]*')"
echo "throttled=$(vcgencmd get_throttled | cut -d= -f2)"
echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
echo "arm_hz=$(vcgencmd measure_clock arm | cut -d= -f2)"
echo "ext5v_v=$(vcgencmd pmic_read_adc EXT5V_V | sed 's/.*=//; s/V$//')"
echo "psu_max_current_ma=$(od -An -tu1 /proc/device-tree/chosen/power/max_current 2>/dev/null | awk '{print $1*16777216+$2*65536+$3*256+$4}')"
echo "fan_rpm=$(cat /sys/devices/platform/cooling_fan/hwmon/*/fan1_input 2>/dev/null)"
echo "loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
echo "eth0_speed_mbps=$(cat /sys/class/net/eth0/speed 2>/dev/null)"
echo "eth0_rx_packets=$(cat /sys/class/net/eth0/statistics/rx_packets)"
echo "eth0_rx_dropped=$(cat /sys/class/net/eth0/statistics/rx_dropped)"
echo "eth0_rx_missed=$(cat /sys/class/net/eth0/statistics/rx_missed_errors)"
echo "eth0_rx_errors=$(cat /sys/class/net/eth0/statistics/rx_errors)"
awk '/^Udp:/{if(!h){split($0,k);h=1}else{split($0,v);for(i=2;i<=NF;i++)print "udp_" k[i] "=" v[i]}}' /proc/net/snmp
for c in /sys/class/drm/card*-HDMI-A-*; do echo "drm_$(basename "$c")=$(cat "$c/status")"; done
mode=$(WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 wlr-randr 2>/dev/null | awk '/^[^ ]/{o=$1} /current/{gsub(/^ +/,""); print o": "$0}' | tr '\n' ';')
echo "wayland_outputs=${mode}"
