"""Display-to-display latency from a phone video that films the laptop panel and the TV together.

Both screens show the same stamped stream: the laptop shows the sender's local preview (or the
desktop that is being captured), the TV shows what the Pi received. Each stamp has a row of 18
square blocks: [white ref][black ref][16 Gray-code bits of the frame ID]. For every camera frame
this tool decodes the ID on both screens; latency = (ID_laptop - ID_tv) / stream_fps.

1) Grab a still to locate the two block rows:
     py -3 tools/camera_latency.py video.mp4 --still 5.0 --out still.png
2) Give each row as x0,y0,x1,y1 = centre of the WHITE reference block and centre of the LAST bit
   block (pixel coordinates in the still); perspective is handled by linear interpolation:
     py -3 tools/camera_latency.py video.mp4 --laptop 102,88,890,95 --tv 1210,400,1650,410 \
         --stream-fps 60 --csv out.csv

Resolution: a reading is quantised to one stream frame (16.7 ms at 60 fps); the camera frame rate
sets how often we sample (240 fps -> every 4.2 ms). Medians over thousands of readings therefore
resolve well below one stream frame, but any single reading is +/- 1 frame.
"""
import argparse
import csv
import statistics
import sys

import cv2
import numpy as np


def parse_row(s):
    x0, y0, x1, y1 = (float(v) for v in s.split(","))
    return x0, y0, x1, y1


def decode(gray_img, row, patch=3):
    x0, y0, x1, y1 = row
    vals = []
    for i in range(18):
        t = i / 17.0
        x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
        xi, yi = int(round(x)), int(round(y))
        p = gray_img[max(0, yi - patch):yi + patch + 1, max(0, xi - patch):xi + patch + 1]
        vals.append(float(p.mean()) if p.size else 0.0)
    white, black = vals[0], vals[1]
    if white - black < 25:  # not enough contrast: screen off / glare / not visible
        return None
    thr = (white + black) / 2
    g = 0
    for v in vals[2:]:
        g = (g << 1) | (1 if v > thr else 0)
    b = g
    shift = 1
    while (g >> shift):
        b ^= g >> shift
        shift += 1
    return b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--still", type=float, help="write the frame at this time (s) and exit")
    ap.add_argument("--out", default="still.png")
    ap.add_argument("--laptop")
    ap.add_argument("--tv")
    ap.add_argument("--stream-fps", type=float, default=60)
    ap.add_argument("--csv", default="")
    a = ap.parse_args()

    cap = cv2.VideoCapture(a.video)
    cam_fps = cap.get(cv2.CAP_PROP_FPS)
    n = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    print(f"video: {n} frames at {cam_fps:.2f} fps ({n / max(cam_fps, 1):.1f} s), "
          f"{int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))}x{int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))}")
    if a.still is not None:
        cap.set(cv2.CAP_PROP_POS_MSEC, a.still * 1000)
        ok, f = cap.read()
        if ok:
            cv2.imwrite(a.out, f)
            print("wrote", a.out)
        return 0
    lap, tv = parse_row(a.laptop), parse_row(a.tv)
    rows, diffs = [], []
    i = 0
    while True:
        ok, f = cap.read()
        if not ok:
            break
        g = cv2.cvtColor(f, cv2.COLOR_BGR2GRAY)
        il, it = decode(g, lap), decode(g, tv)
        t = i / cam_fps if cam_fps else i
        d = None
        if il is not None and it is not None:
            d = (il - it) % 65536
            if d > 32768:
                d -= 65536
        rows.append((i, round(t, 4), il, it, d))
        if d is not None and -2 <= d <= 600:
            diffs.append(d)
        i += 1
    if a.csv:
        with open(a.csv, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["cam_frame", "t_s", "id_laptop", "id_tv", "diff_frames"])
            w.writerows(rows)
    valid = len(diffs)
    print(f"decoded both screens in {valid}/{len(rows)} camera frames")
    if not diffs:
        return 1
    ms = [d * 1000.0 / a.stream_fps for d in diffs]
    ms.sort()
    p95 = ms[min(len(ms) - 1, int(0.95 * (len(ms) - 1)))]
    print(f"display-to-display latency: median {statistics.median(ms):.1f} ms, mean {statistics.mean(ms):.1f} ms, "
          f"p95 {p95:.1f} ms, min {ms[0]:.1f} ms, max {ms[-1]:.1f} ms")
    print(f"granularity: one stream frame = {1000 / a.stream_fps:.1f} ms per reading; camera sample interval "
          f"{1000 / cam_fps:.1f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
