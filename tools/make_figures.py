"""Regenerate the README figures from results/summary.csv (and a pattern frame dump).

  py -3 tools/make_figures.py
"""
import collections
import csv
import pathlib
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "assets"
OUT.mkdir(exist_ok=True)
BG, FG, MUTED, ACCENT, ACCENT2, ACCENT3 = "#0f1115", "#e8e8e8", "#8a8f98", "#3ddc84", "#4ea1ff", "#f5b041"

MODES = [
    ("A1-pattern-960x540p60-raw", "Raw RGB 960×540@60\ngenerated pattern"),
    ("A2-desktop-test-960x540p60-raw", "Raw RGB 960×540@60\ndesktop, moving"),
    ("A3-desktop-idle-960x540p60-raw", "Raw RGB 960×540@60\ndesktop, idle"),
    ("B-pattern-1080p30-ffv1", "FFV1 1080p30\ngenerated pattern"),
    ("B-desktop-idle-1080p30-ffv1", "FFV1 1080p30\ndesktop, idle"),
    ("B-desktop-text-1080p30-ffv1", "FFV1 1080p30\ndesktop, scrolling text"),
]


def load():
    g = collections.defaultdict(list)
    with (ROOT / "results" / "summary.csv").open(newline="") as f:
        for r in csv.DictReader(f):
            g[r["label"]].append(r)
    return g


def mean(rows, k):
    v = [float(r[k]) for r in rows if r.get(k) not in (None, "")]
    return statistics.mean(v) if v else 0.0


def style(ax, fig):
    fig.patch.set_facecolor(BG)
    ax.set_facecolor(BG)
    for s in ax.spines.values():
        s.set_visible(False)
    ax.tick_params(colors=FG, labelsize=10)
    ax.xaxis.label.set_color(MUTED)
    ax.grid(axis="x", color="#262a33", linewidth=0.8)
    ax.set_axisbelow(True)


def latency_chart(g):
    labels, transport, t95, e2e, e95 = [], [], [], [], []
    for key, lab in MODES:
        rows = g.get(key)
        if not rows:
            continue
        labels.append(lab)
        transport.append(mean(rows, "lat_pay_complete_med_ms"))
        t95.append(mean(rows, "lat_pay_complete_p95_ms"))
        e2e.append(mean(rows, "lat_src_verified_med_ms"))
        e95.append(mean(rows, "lat_src_verified_p95_ms"))
    fig, ax = plt.subplots(figsize=(10, 5.2), dpi=150)
    style(ax, fig)
    y = range(len(labels))
    h = 0.36
    ax.barh([i - h / 2 for i in y], e2e, h, color=ACCENT2, label="sender frame ready → verified on the Pi (median)")
    ax.barh([i + h / 2 for i in y], transport, h, color=ACCENT, label="payloader → last packet on the Pi (median)")
    for i, (a, a95, b, b95) in enumerate(zip(e2e, e95, transport, t95)):
        for val, p95, yy in ((a, a95, i - h / 2), (b, b95, i + h / 2)):
            ax.plot([val, p95], [yy, yy], color=MUTED, linewidth=1.2, zorder=3)
            ax.scatter([p95], [yy], color=FG, s=10, zorder=4)
            ax.text(max(val, p95) + 1.2, yy, f"{val:.1f} ms  (p95 {p95:.1f})", va="center", color=FG, fontsize=8.5)
    ax.set_xlim(0, max(e95 + t95) * 1.35)
    ax.set_yticks(list(y))
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.set_xlabel("milliseconds (clock-synced, uncertainty < ±1 ms)  ·  whisker = p95")
    ax.set_title("Where a frame is after it leaves the laptop", color=FG, loc="left", fontsize=14, pad=30)
    leg = ax.legend(loc="lower left", bbox_to_anchor=(0, 1.0), frameon=False, fontsize=9, ncol=2)
    for t in leg.get_texts():
        t.set_color(FG)
    fig.tight_layout()
    fig.savefig(OUT / "latency.png", facecolor=BG)
    plt.close(fig)


def budget_chart(g):
    keys = [k for k in MODES if k[0].startswith("B-")] + [MODES[0]]
    labels, enc, wire, dec, rest = [], [], [], [], []
    for key, lab in keys:
        rows = g.get(key)
        if not rows:
            continue
        e = mean(rows, "enc_ms_med")
        w = mean(rows, "lat_pay_complete_med_ms")
        d = mean(rows, "decode_ms_med")
        tot = mean(rows, "lat_src_verified_med_ms")
        labels.append(lab)
        enc.append(e)
        wire.append(w)
        dec.append(d)
        rest.append(max(0.0, tot - e - w - d))
    fig, ax = plt.subplots(figsize=(10, 4.2), dpi=150)
    style(ax, fig)
    y = list(range(len(labels)))
    left = [0.0] * len(y)
    for vals, col, name in [(enc, ACCENT3, "encode (laptop)"), (wire, ACCENT, "on the wire"),
                            (dec, "#e05d5d", "FFV1 decode (Pi)"), (rest, "#5a6170", "queues, hashing, hand-offs")]:
        ax.barh(y, vals, left=left, color=col, label=name, height=0.55)
        left = [l + v for l, v in zip(left, vals)]
    for i, t in enumerate(left):
        ax.text(t + 0.8, i, f"{t:.1f} ms", va="center", color=FG, fontsize=9)
    ax.set_yticks(y)
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.set_xlabel("median milliseconds per frame")
    ax.set_title("The Pi's FFV1 decoder is the slow part, not the network", color=FG, loc="left", fontsize=14, pad=14)
    leg = ax.legend(loc="lower right", frameon=False, fontsize=9, ncol=2)
    for t in leg.get_texts():
        t.set_color(FG)
    fig.tight_layout()
    fig.savefig(OUT / "budget.png", facecolor=BG)
    plt.close(fig)


if __name__ == "__main__":
    data = load()
    latency_chart(data)
    budget_chart(data)
    print("wrote", OUT / "latency.png", OUT / "budget.png")
