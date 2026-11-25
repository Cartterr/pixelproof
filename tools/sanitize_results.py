"""Strip machine-specific paths (Windows profile, Pi home) from results/ before publishing."""
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent / "results"
win_project = re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+[^\\/,;\"]+[\\/]+(?:[^\\/,;\"]+[\\/]+)*?stream-bench[\\/]+")
win_profile = re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+[^\\/,;\"]+")
pi_home = re.compile(r"/home/[a-z0-9_-]+/")
n = 0
for p in root.rglob("*"):
    if p.is_file() and p.suffix in (".csv", ".log", ".txt"):
        s = p.read_text(encoding="utf-8", errors="replace")
        t = pi_home.sub("~/", win_profile.sub("%USERPROFILE%", win_project.sub("", s)))
        if t != s:
            p.write_text(t, encoding="utf-8")
            n += 1
print(f"sanitized {n} files")
