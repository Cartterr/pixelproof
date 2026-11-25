"""Wired-link check: route, ping both ways, negotiated speed, iperf3 TCP/UDP both directions.

  py -3 scripts/link-check.py --tx-ip 10.77.0.1 --rx-ip 10.77.0.2 [--out results/link-check.txt]

iperf3 always runs as a client on Windows (server on the Pi), so no inbound rule is needed on the
laptop; -R covers the Pi->laptop direction.
"""
import argparse
import pathlib
import subprocess
import sys
import time


def run(cmd, timeout=60, shell=False):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, shell=shell)
    return (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tx-ip", required=True)
    ap.add_argument("--rx-ip", required=True)
    ap.add_argument("--pi", default="pi5")
    ap.add_argument("--out", default="")
    ap.add_argument("--udp-mbps", type=int, default=950)
    a = ap.parse_args()
    out = []

    def log(title, text):
        block = f"### {title}\n{text}\n"
        print(block, flush=True)
        out.append(block)

    log("route (Windows)", run(["powershell", "-NoProfile", "-Command",
        f"Find-NetRoute -RemoteIPAddress {a.rx_ip} | Select-Object -First 2 InterfaceAlias,IPAddress,DestinationPrefix,NextHop | Format-List"]))
    log("adapter (Windows)", run(["powershell", "-NoProfile", "-Command",
        f"$i=(Find-NetRoute -RemoteIPAddress {a.rx_ip} | Select-Object -First 1).InterfaceAlias; "
        "Get-NetAdapter -Name $i | Select-Object Name,InterfaceDescription,Status,LinkSpeed,DriverVersion | Format-List"]))
    log("ping laptop -> Pi", run(["ping", "-n", "10", "-S", a.tx_ip, a.rx_ip]))
    log("Pi: link + route + ping Pi -> laptop", run(["ssh", a.pi,
        f"/usr/sbin/ethtool eth0 | grep -E 'Speed|Duplex|Link detected'; ip route get {a.tx_ip}; ping -c 10 -I {a.rx_ip} {a.tx_ip} | tail -3"]))
    run(["ssh", a.pi, "pkill -x iperf3; true"])
    run(["ssh", a.pi, f"setsid nohup iperf3 -s -B {a.rx_ip} > /tmp/iperf3-s.log 2>&1 < /dev/null &"])
    time.sleep(1)
    for title, args in [
        ("iperf3 TCP laptop -> Pi (10 s)", ["-t", "10"]),
        ("iperf3 TCP Pi -> laptop (10 s)", ["-t", "10", "-R"]),
        (f"iperf3 UDP laptop -> Pi {a.udp_mbps} Mb/s, 1400 B (10 s)", ["-u", "-b", f"{a.udp_mbps}M", "-l", "1400", "-t", "10"]),
    ]:
        txt = run(["iperf3", "-c", a.rx_ip, "-B", a.tx_ip, "-f", "m"] + args, timeout=40)
        log(title, "\n".join(l for l in txt.splitlines() if "sender" in l or "receiver" in l or "error" in l.lower()) or txt[-600:])
    run(["ssh", a.pi, "pkill -x iperf3; true"])
    if a.out:
        pathlib.Path(a.out).write_text("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
