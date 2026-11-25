#!/bin/bash
# Temporary addressing for a DIRECT laptop<->Pi cable (Pi side). Not persistent: gone on reboot.
#   sudo ./link-setup-pi.sh            # eth0 -> static 10.77.0.2/24, no gateway (runtime only)
#   sudo ./link-setup-pi.sh --remove   # re-apply the saved (DHCP) eth0 connection
# Uses `nmcli device modify`, which changes only the ACTIVE connection in memory: the saved
# NetworkManager/netplan profile is untouched, and NetworkManager no longer wipes the address when
# DHCP fails on a direct cable (a plain `ip addr add` gets flushed). Wi-Fi/Tailscale are untouched.
# Also raises the UDP receive-buffer ceiling for this boot so sb_receiver can get a 32 MiB buffer.
set -euo pipefail
IP=${IP:-10.77.0.2/24}
DEV=${DEV:-eth0}
if [ "${1:-}" = "--remove" ]; then
  nmcli device reapply "$DEV" 2>/dev/null || nmcli device connect "$DEV"
  echo "restored saved connection on $DEV"
  exit 0
fi
nmcli device modify "$DEV" ipv4.method manual ipv4.addresses "$IP" ipv4.gateway "" ipv4.never-default yes >/dev/null
/usr/sbin/sysctl -qw net.core.rmem_max=67108864 net.core.wmem_max=67108864
sleep 1
ip -br addr show dev "$DEV"
/usr/sbin/ethtool "$DEV" 2>/dev/null | grep -E "Speed|Duplex|Link detected"
ip route | grep -E "^default"
