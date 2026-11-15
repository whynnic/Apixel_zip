#!/usr/bin/env python3
"""Turns bench.py CSV files into Markdown tables. Usage: summary.py results/*.csv > table.md"""
import csv, sys
from collections import defaultdict

rows = []
for path in sys.argv[1:]:
    with open(path, newline="") as f:
        rows += list(csv.DictReader(f))

SINGLE = {"enwik8"}  # reported separately from the Silesia files
tools = []
data = defaultdict(dict)
for r in rows:
    if r["tool"] not in tools:
        tools.append(r["tool"])
    data[r["tool"]][r["file"]] = r
files = sorted({r["file"] for r in rows if r["file"] not in SINGLE})


def total(tool, names):
    d = [data[tool][n] for n in names if n in data[tool]]
    if len(d) != len(names):
        return None
    raw = sum(int(x["raw"]) for x in d)
    size = sum(int(x["size"]) for x in d)
    ct = sum(float(x["c_sec"]) for x in d)
    dt = sum(float(x["d_sec"]) for x in d)
    mem = max(max(int(x["c_mib"]), int(x["d_mib"])) for x in d)
    return raw, size, raw / 1048576 / ct, raw / 1048576 / dt, mem


def table(title, names):
    print(f"### {title}\n")
    print("| Archiver | Size, bytes | % of original | Compress, MiB/s | Decompress, MiB/s | Memory, MiB |")
    print("|---|---:|---:|---:|---:|---:|")
    res = [(t, total(t, names)) for t in tools]
    for t, v in sorted((x for x in res if x[1]), key=lambda x: x[1][1]):
        raw, size, cs, ds, mem = v
        print(f"| {t} | {size:,} | {100 * size / raw:.2f} | {cs:.2f} | {ds:.1f} | {mem} |".replace(",", " "))
    print()


table(f"Silesia, {len(files)} files, total", files)
for s in sorted(SINGLE):
    table(s, [s])

ref = [t for t in ("apixel", "7z-ultra", "bzip3-b32", "bsc-b32-e2", "zstd-22-long") if t in data]
print("### Per file, bytes\n")
print("| File | " + " | ".join(ref) + " | apixel vs 7z-ultra |")
print("|---|" + "---:|" * (len(ref) + 1))
wins = 0
allf = files + sorted(SINGLE)
for n in allf:
    cells = [f"{int(data[t][n]['size']):,}".replace(",", " ") if n in data[t] else "-" for t in ref]
    a, z = int(data["apixel"][n]["size"]), int(data["7z-ultra"][n]["size"])
    wins += a < z
    print(f"| {n} | " + " | ".join(cells) + f" | {100 * (a - z) / z:+.1f}% |")
print(f"\nApixel is smaller than 7-Zip Ultra on {wins} files out of {len(allf)}.")
