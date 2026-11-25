"""Run the Pass A (raw RGB) and Pass B (FFV1) matrices with bench.py.

  py -3 scripts/passes.py --tx-ip 10.77.0.1 --rx-ip 10.77.0.2 --pass A
  py -3 scripts/passes.py --tx-ip 10.77.0.1 --rx-ip 10.77.0.2 --pass B
  options: --samples 3 --duration 30 --warmup 5 --display wayland --panel \\\\.\\DISPLAY1 --only <label-substring>

Desktop scenes are produced by a second sb_sender instance that shows a generated scene full
screen on the laptop panel (no network); the benchmarked sender then captures that real
desktop with Desktop Duplication. Scenes: idle (the desktop as it is), text (scrolling document
page), test (high-motion test pattern).
"""
import argparse
import csv
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
SENDER = ROOT / "build" / "win" / "Release" / "sb_sender.exe"
GST_BIN = os.path.join(os.environ.get("GSTREAMER_1_0_ROOT_MSVC_X86_64") or r"C:\Program Files\gstreamer\1.0\msvc_x86_64", "bin")


def modes(p):
    if p == "A":
        return [
            dict(label="A1-pattern-960x540p60-raw", codec="raw", source="pattern", w=960, h=540, fps=60),
            dict(label="A2-desktop-test-960x540p60-raw", codec="raw", source="desktop", scene="test", w=960, h=540, fps=60),
            dict(label="A3-desktop-idle-960x540p60-raw", codec="raw", source="desktop", scene="idle", w=960, h=540, fps=60),
        ]
    if p == "A720":
        return [
            dict(label="A4-pattern-1280x720p30-raw", codec="raw", source="pattern", w=1280, h=720, fps=30),
            dict(label="A5-desktop-test-1280x720p30-raw", codec="raw", source="desktop", scene="test", w=1280, h=720, fps=30),
        ]
    if p in ("B", "B30", "B60"):
        out = []
        for fps in ([30, 60] if p == "B" else [int(p[1:])]):
            out += [
                dict(label=f"B-pattern-1080p{fps}-ffv1", codec="ffv1", source="pattern", w=1920, h=1080, fps=fps),
                dict(label=f"B-desktop-idle-1080p{fps}-ffv1", codec="ffv1", source="desktop", scene="idle", w=1920, h=1080, fps=fps),
                dict(label=f"B-desktop-text-1080p{fps}-ffv1", codec="ffv1", source="desktop", scene="text", w=1920, h=1080, fps=fps),
                dict(label=f"B-desktop-test-1080p{fps}-ffv1", codec="ffv1", source="desktop", scene="test", w=1920, h=1080, fps=fps),
            ]
        return out
    raise SystemExit(f"unknown pass {p}")


def start_scene(scene, panel, env):
    if scene in (None, "idle"):
        return None
    cmd = [str(SENDER), "--source", "pattern", "--scene", scene, "--width", "2560", "--height", "1600",
           "--fps", "60", "--preview-monitor", panel, "--warmup", "0", "--duration", "0"]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    time.sleep(2)
    return p


def stable(rows, fps):
    for r in rows:
        try:
            exp, ok, okf = int(r["expected"]), int(r["ok"]), float(r["ok_fps"])
        except (KeyError, ValueError):
            return False
        if exp == 0 or (exp - ok) / exp > 0.01 or okf < 0.95 * fps:
            return False
    return bool(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tx-ip", required=True)
    ap.add_argument("--rx-ip", required=True)
    ap.add_argument("--pass", dest="pas", required=True, choices=["A", "A720", "B", "B30", "B60"])
    ap.add_argument("--samples", type=int, default=3)
    ap.add_argument("--duration", type=float, default=30)
    ap.add_argument("--warmup", type=float, default=5)
    ap.add_argument("--display", default="none")
    ap.add_argument("--panel", default=r"\\.\DISPLAY1", help="monitor that shows the scene and is captured")
    ap.add_argument("--only", default="")
    ap.add_argument("--perframe", action="store_true")
    ap.add_argument("--allow-heavy", action="store_true",
                    help="also run FFV1 + full-screen high-motion scene + desktop capture (crashed the test laptop once)")
    a = ap.parse_args()
    env = dict(os.environ)
    env["PATH"] = GST_BIN + os.pathsep + env["PATH"]

    ms = [m for m in modes(a.pas) if a.only in m["label"]]
    if not a.allow_heavy:
        # A full-screen 2560x1600 preview + Desktop Duplication of the same panel + FFV1 encode
        # hard-reset the reference laptop once (see RESULTS.md, Incident). Opt in explicitly.
        skipped = [m["label"] for m in ms if m["codec"] == "ffv1" and m.get("scene") == "test"]
        ms = [m for m in ms if m["label"] not in skipped]
        if skipped:
            print("skipping (needs --allow-heavy):", ", ".join(skipped), flush=True)
    per = a.samples * (a.warmup + a.duration + 12)
    print(f"Pass {a.pas}: {len(ms)} modes x {a.samples} samples x ({a.warmup:.0f}+{a.duration:.0f}) s "
          f"~ {len(ms) * per / 60:.0f} min", flush=True)
    sumf = ROOT / "results" / "summary.csv"
    for m in ms:
        scene = m.get("scene")
        proc = start_scene(scene, a.panel, env)
        cmd = [sys.executable, str(ROOT / "scripts" / "bench.py"), "--label", m["label"], "--codec", m["codec"],
               "--source", m["source"], "--width", str(m["w"]), "--height", str(m["h"]), "--fps", str(m["fps"]),
               "--tx-ip", a.tx_ip, "--rx-ip", a.rx_ip, "--samples", str(a.samples), "--warmup", str(a.warmup),
               "--duration", str(a.duration), "--display", a.display]
        if m["source"] == "desktop":
            cmd += ["--sender-args", f"--monitor {a.panel.replace(chr(92), '/')}"]
        if a.perframe:
            cmd += ["--perframe"]
        try:
            subprocess.run(cmd, check=False)
        finally:
            if proc:
                proc.terminate()
                proc.wait(timeout=10)
        if a.pas == "A" and m["label"].startswith("A1"):
            rows = [r for r in csv.DictReader(sumf.open()) if r["label"] == m["label"]][-a.samples:]
            if not stable(rows, m["fps"]):
                print("A1 unstable at 960x540p60 -> also running the 1280x720p30 fallback", flush=True)
                ms += modes("A720")
    return 0


if __name__ == "__main__":
    sys.exit(main())
