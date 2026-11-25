"""Benchmark runner (Windows sender host -> Pi receiver over SSH).

For each sample: snapshot Pi state, start sb_receiver on the Pi, run sb_sender locally, wait for
the receiver to finish its accounting, snapshot Pi state again, copy every log/CSV into
results/<run>/sample_<n>/ and append one combined row to results/summary.csv.

Example (Pass A, generated pattern, 3 x 30 s after 5 s warm-up):
  py -3 scripts/bench.py --label A-pattern-960x540p60 --codec raw --source pattern \
      --width 960 --height 540 --fps 60 --tx-ip 10.77.0.1 --rx-ip 10.77.0.2 --samples 3
"""
import argparse
import csv
import datetime as dt
import os
import pathlib
import shlex
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
GST_BIN = os.path.join(os.environ.get("GSTREAMER_1_0_ROOT_MSVC_X86_64") or r"C:\Program Files\gstreamer\1.0\msvc_x86_64", "bin")
SENDER = ROOT / "build" / "win" / "Release" / "sb_sender.exe"
PI_DIR = "~/stream-bench"
PI_RES = "~/sb-results"


def ssh(host: str, cmd: str, check=True, timeout=60) -> str:
    r = subprocess.run(["ssh", host, cmd], capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"ssh {cmd!r} failed: {r.stderr.strip()}")
    return r.stdout


def parse_kv(text: str) -> dict:
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def read_last_row(path: pathlib.Path) -> dict:
    if not path.exists():
        return {}
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    return rows[-1] if rows else {}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--label", required=True)
    ap.add_argument("--codec", default="raw", choices=["raw", "ffv1"])
    ap.add_argument("--source", default="pattern")
    ap.add_argument("--width", type=int, default=960)
    ap.add_argument("--height", type=int, default=540)
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--warmup", type=float, default=5)
    ap.add_argument("--duration", type=float, default=30)
    ap.add_argument("--tx-ip", required=True, help="laptop wired address (sender binds here)")
    ap.add_argument("--rx-ip", required=True, help="Pi wired address (receiver binds here)")
    ap.add_argument("--pi", default="pi5", help="ssh host alias for the Pi (management path)")
    ap.add_argument("--display", default="none", help="receiver display: none|wayland|kms|gl|fake")
    ap.add_argument("--rtp-path", default="app")
    ap.add_argument("--perframe", action="store_true", help="per-frame CSVs on both ends")
    ap.add_argument("--sender-args", default="", help="extra sender args")
    ap.add_argument("--receiver-args", default="", help="extra receiver args")
    ap.add_argument("--pause", type=float, default=3, help="seconds between samples")
    a = ap.parse_args()

    run = dt.datetime.now().strftime("%Y%m%d-%H%M%S") + "_" + a.label
    run_dir = ROOT / "results" / run
    run_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = GST_BIN + os.pathsep + env["PATH"]
    summary_rows = []

    for n in range(1, a.samples + 1):
        sd = run_dir / f"sample_{n}"
        sd.mkdir(exist_ok=True)
        rdir = f"{PI_RES}/{run}/sample_{n}"
        print(f"== {a.label} sample {n}/{a.samples}: {a.codec} {a.source} {a.width}x{a.height}@{a.fps}, "
              f"{a.warmup:.0f}s warm-up + {a.duration:.0f}s", flush=True)
        ssh(a.pi, f"mkdir -p {rdir} && pkill -x sb_receiver; true", check=False)
        before = ssh(a.pi, f"bash {PI_DIR}/scripts/pi-sysinfo.sh")
        (sd / "pi_before.txt").write_text(before)
        rx_cmd = (
            # Only the receiver itself is backgrounded (with all fds redirected) so ssh returns at once.
            f"cd {PI_DIR} || exit 1; export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0; "
            f"setsid nohup ./build/pi/sb_receiver --mode {a.codec} --rtp-path {a.rtp_path} "
            f"--width {a.width} --height {a.height} --fps {a.fps} --bind {a.rx_ip} --display {a.display} "
            f"--duration {a.duration} --label {shlex.quote(a.label)} --csv {rdir}/receiver.csv "
            f"--timeseries {rdir}/receiver_ts.csv "
            + (f"--perframe {rdir}/receiver_frames.csv " if a.perframe else "")
            + f"{a.receiver_args} > {rdir}/receiver.log 2>&1 < /dev/null & echo $!"
        )
        pid = ssh(a.pi, rx_cmd).strip()
        for _ in range(50):
            if "pipeline" in ssh(a.pi, f"cat {rdir}/receiver.log", check=False):
                break
            time.sleep(0.1)
        time.sleep(0.5)

        cmd = [str(SENDER), "--source", a.source, "--codec", a.codec, "--width", str(a.width),
               "--height", str(a.height), "--fps", str(a.fps), "--dest", a.rx_ip, "--bind", a.tx_ip,
               "--warmup", str(a.warmup), "--duration", str(a.duration), "--label", a.label,
               "--csv", str(sd / "sender.csv"), "--timeseries", str(sd / "sender_ts.csv")]
        if a.perframe:
            cmd += ["--perframe", str(sd / "sender_frames.csv")]
        cmd += shlex.split(a.sender_args)
        with (sd / "sender.log").open("w") as log:
            rc = subprocess.run(cmd, stderr=log, stdout=log, env=env,
                                timeout=a.warmup + a.duration + 60).returncode
        # Receiver exits ~1 s after the END message; give it time to write its summary.
        for _ in range(100):
            if ssh(a.pi, f"kill -0 {pid} 2>/dev/null && echo run || echo done", check=False).strip() == "done":
                break
            time.sleep(0.2)
        else:
            ssh(a.pi, f"kill -INT {pid}", check=False)
            time.sleep(2)
        after = ssh(a.pi, f"bash {PI_DIR}/scripts/pi-sysinfo.sh")
        (sd / "pi_after.txt").write_text(after)
        subprocess.run(["scp", "-q", f"{a.pi}:{rdir}/*", str(sd)], check=False)

        tx = read_last_row(sd / "sender.csv")
        rx = read_last_row(sd / "receiver.csv")
        b, e = parse_kv(before), parse_kv(after)

        def delta(k):
            try:
                return str(int(e.get(k, 0)) - int(b.get(k, 0)))
            except ValueError:
                return ""

        row = {
            "run": run, "sample": n, "label": a.label, "codec": a.codec, "source": a.source,
            "width": a.width, "height": a.height, "fps_nominal": a.fps, "display": a.display,
            "sender_rc": rc, "fps_sent": tx.get("fps_sent", ""), "sender_queue_drops": tx.get("sender_queue_drops", ""),
            "payload_mbps": tx.get("payload_mbps", ""), "wire_mbps_est": tx.get("wire_mbps_est", ""),
            "enc_ms_med": tx.get("enc_ms_med", ""), "enc_ms_p95": tx.get("enc_ms_p95", ""),
            "sender_cpu_1core_med": tx.get("cpu_proc_pct_one_core_med", ""),
            "expected": rx.get("expected", ""), "ok": rx.get("ok", ""), "ok_fps": rx.get("ok_fps", ""),
            "corrupted": rx.get("corrupted", ""), "incomplete": rx.get("incomplete", ""),
            "never_arrived": rx.get("never_arrived", ""), "rx_dropped": rx.get("rx_dropped", ""),
            "duplicates": rx.get("duplicates", ""), "delivery_failures": rx.get("delivery_failures", ""),
            "packets_lost": rx.get("packets_lost", ""),
            "lat_pay_complete_med_ms": rx.get("lat_pay_complete_med_ms", ""),
            "lat_pay_complete_p95_ms": rx.get("lat_pay_complete_p95_ms", ""),
            "lat_src_verified_med_ms": rx.get("lat_src_verified_med_ms", ""),
            "lat_src_verified_p95_ms": rx.get("lat_src_verified_p95_ms", ""),
            "decode_ms_med": rx.get("decode_ms_med", ""), "decode_ms_p95": rx.get("decode_ms_p95", ""),
            "sync_unc_ms_max": rx.get("sync_unc_ms_max", ""),
            "receiver_cpu_1core_med": rx.get("cpu_proc_pct_one_core_med", ""),
            "receiver_sys_cpu_med": rx.get("cpu_sys_pct_med", ""),
            "display_frames": rx.get("display_frames", ""),
            "eth0_rx_dropped_delta": delta("eth0_rx_dropped"), "eth0_rx_missed_delta": delta("eth0_rx_missed"),
            "udp_rcvbuf_errors_delta": delta("udp_RcvbufErrors"), "udp_in_errors_delta": delta("udp_InErrors"),
            "pi_temp_start": b.get("temp_c", ""), "pi_temp_end": e.get("temp_c", ""),
            "pi_throttled_start": b.get("throttled", ""), "pi_throttled_end": e.get("throttled", ""),
            "pi_governor": b.get("governor", ""), "pi_arm_hz_end": e.get("arm_hz", ""),
            "pi_ext5v_start": b.get("ext5v_v", ""), "pi_psu_max_ma": b.get("psu_max_current_ma", ""),
            "pi_fan_rpm_end": e.get("fan_rpm", ""), "eth0_speed": b.get("eth0_speed_mbps", ""),
            "pi_display": b.get("wayland_outputs", ""),
        }
        summary_rows.append(row)
        sumf = ROOT / "results" / "summary.csv"
        new = not sumf.exists() or sumf.stat().st_size == 0
        with sumf.open("a", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(row.keys()))
            if new:
                w.writeheader()
            w.writerow(row)
        print(f"   sent {row['fps_sent']} fps, ok {row['ok']}/{row['expected']} ({row['ok_fps']} fps), "
              f"corrupt {row['corrupted']}, incomplete {row['incomplete']}, never {row['never_arrived']}, "
              f"rxdrop {row['rx_dropped']}, lost pkts {row['packets_lost']}, "
              f"pay->rx med {row['lat_pay_complete_med_ms']} p95 {row['lat_pay_complete_p95_ms']} ms, "
              f"udp rcvbuf err +{row['udp_rcvbuf_errors_delta']}, temp {row['pi_temp_start']}->{row['pi_temp_end']}C "
              f"throttled {row['pi_throttled_end']}", flush=True)
        if n < a.samples:
            time.sleep(a.pause)
    print(f"results in {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
