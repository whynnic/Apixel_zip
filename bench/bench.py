#!/usr/bin/env python3
"""Apixel benchmark stand.

Compresses every file with every tool, decompresses, verifies SHA-256 and
records size, times and peak memory. Multi-threading can be controlled via
--threads N; tools without MT support are marked accordingly in CSV.

Usage:
  bench.py --apixel build/apixel --out results.csv [--threads N] [--tool-dir DIR] [--tools a,b,...] FILE...
Tools that are not installed are skipped with a note.
"""
import argparse, csv, hashlib, os, re, shutil, subprocess, sys, tempfile, time

# Rival tools built outside the repo: $APIXEL_TOOL_DIR, else ../tools-ext/bin next to the checkout.
# No absolute personal path here: the repository is meant to be public.
DEFAULT_TOOL_DIR = os.environ.get("APIXEL_TOOL_DIR") or os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools-ext", "bin"))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, stdout_path=None):
    """Runs cmd; returns (seconds, peak_rss_mib). Raises on non-zero exit."""
    out = open(stdout_path, "wb") if stdout_path else subprocess.DEVNULL
    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.DEVNULL)
    _, status, ru = os.wait4(p.pid, 0)
    dt = time.perf_counter() - t0
    if stdout_path:
        out.close()
    if status != 0:
        raise RuntimeError("command failed: " + " ".join(cmd))
    return dt, ru.ru_maxrss / 1024.0


def resolve_binary(name_or_path, tool_dir=None):
    """Finds binary executable by explicit path, in tool_dir, or in PATH."""
    if os.path.dirname(name_or_path):
        if os.path.isfile(name_or_path) and os.access(name_or_path, os.X_OK):
            return os.path.abspath(name_or_path)
        if os.path.exists(name_or_path):
            return os.path.abspath(name_or_path)
        # Fallbacks for default build path of apixel
        if name_or_path == "build/apixel":
            for cand in ("build-g-t6/apixel", "build-t6/apixel"):
                if os.path.isfile(cand) and os.access(cand, os.X_OK):
                    return os.path.abspath(cand)
        return None

    # Check specified tool_dir first
    if tool_dir and os.path.isdir(tool_dir):
        p = os.path.join(tool_dir, name_or_path)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return os.path.abspath(p)

    # Check default tool_dir if different from tool_dir
    if tool_dir != DEFAULT_TOOL_DIR and os.path.isdir(DEFAULT_TOOL_DIR):
        p = os.path.join(DEFAULT_TOOL_DIR, name_or_path)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return os.path.abspath(p)

    # Check system PATH
    found = shutil.which(name_or_path)
    if found:
        return os.path.abspath(found)

    return None


def parse_version(raw):
    """Extracts a concise version string from tool banner/help output."""
    for line in raw.splitlines():
        line = line.strip()
        if not line or "error" in line.lower() or "unknown switch" in line.lower():
            continue
        m = re.search(r"7-Zip\s*(?:\([a-zA-Z0-9]+\)\s*)?(?:\[\d+\]\s*)?(\d+\.\d+)", line)
        if m:
            return m.group(1)
        m = re.search(r"Zstandard.*?v(\d+\.\d+\.\d+)", line)
        if m:
            return m.group(1)
        m = re.search(r"This is bsc.*?Version\s*([0-9a-zA-Z\.\-]+)", line)
        if m:
            return m.group(1).rstrip(",")
        m = re.search(r"Kanzi\s+([0-9a-zA-Z\.\-]+)", line)
        if m:
            return m.group(1)
        m = re.search(r"zpaq\s+v?([0-9a-zA-Z\.\-]+)", line)
        if m:
            return m.group(1)
        m = re.search(r"paq8px\s+v?([0-9a-zA-Z\.\-]+)", line)
        if m:
            return m.group(1)
        m = re.search(r"apixel\s+([0-9a-zA-Z\.\-]+)", line)
        if m:
            return m.group(1)
        m = re.search(r"(?:xz|brotli|bzip3)\s+(?:.*?)([0-9]+\.[0-9]+(?:\.[0-9]+)?)", line)
        if m:
            return m.group(1)
        m = re.search(r"\bv?(\d+\.\d+(?:\.\d+)?)\b", line)
        if m:
            return m.group(1)
    return None


_VERSION_CACHE = {}


def query_tool_version(binary_path):
    """Executes the tool once to extract its version string."""
    if binary_path in _VERSION_CACHE:
        return _VERSION_CACHE[binary_path]
    version = "unknown"
    for cmd in [[binary_path, "--version"], [binary_path, "-V"], [binary_path, "-v"], [binary_path, "--help"], [binary_path, "-h"], [binary_path]]:
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=2)
            out = p.stdout.strip() or p.stderr.strip()
            if out:
                ver = parse_version(out)
                if ver:
                    version = ver
                    break
        except Exception:
            continue
    _VERSION_CACHE[binary_path] = version
    return version


def tools(apixel, bsc, threads):
    """Returns mapping of tool name to specification dict:
       binary, has_threads, comp(i, a), decomp(a, o), max_file_size (optional).
    """
    th_s = str(threads)
    bsc_th = ["-t", "-T"] if threads == 1 else [f"-T{threads}"]

    return {
        "apixel": {
            "binary": apixel,
            "has_threads": True,
            "comp": lambda i, a: ([apixel, "c", "-q", "-f", "-T", th_s, i, "-o", a], None),
            "decomp": lambda a, o: ([apixel, "x", "-q", "-f", "-T", th_s, a, "-o", o], None),
        },
        "apixel-nv": {
            "binary": apixel,
            "has_threads": True,
            "comp": lambda i, a: ([apixel, "c", "-q", "-f", "-T", th_s, "--no-verify", i, "-o", a], None),
            "decomp": lambda a, o: ([apixel, "x", "-q", "-f", "-T", th_s, a, "-o", o], None),
        },
        "7z-ultra": {
            "binary": "7z",
            "has_threads": True,
            "comp": lambda i, a: (["7z", "a", "-t7z", "-mx=9", f"-mmt={th_s}", "-bso0", "-bsp0", a, i], None),
            "decomp": lambda a, o: (["7z", "e", f"-mmt={th_s}", "-so", "-bso0", "-bsp0", a], o),
        },
        "xz-9": {
            "binary": "xz",
            "has_threads": True,
            "comp": lambda i, a: (["xz", "-9", f"-T{th_s}", "-k", "-c", i], a),
            "decomp": lambda a, o: (["xz", "-d", f"-T{th_s}", "-c", a], o),
        },
        "xz-9e": {
            "binary": "xz",
            "has_threads": True,
            "comp": lambda i, a: (["xz", "-9e", f"-T{th_s}", "-k", "-c", i], a),
            "decomp": lambda a, o: (["xz", "-d", f"-T{th_s}", "-c", a], o),
        },
        "zstd-19": {
            "binary": "zstd",
            "has_threads": True,
            "comp": lambda i, a: (["zstd", "-19", f"-T{th_s}", "-q", "-f", i, "-o", a], None),
            "decomp": lambda a, o: (["zstd", "-d", "-q", "-f", a, "-o", o], None),
        },
        "zstd-22-long": {
            "binary": "zstd",
            "has_threads": True,
            "comp": lambda i, a: (["zstd", "--ultra", "-22", "--long=27", f"-T{th_s}", "-q", "-f", i, "-o", a], None),
            "decomp": lambda a, o: (["zstd", "-d", "--long=27", "-q", "-f", a, "-o", o], None),
        },
        "brotli-11": {
            "binary": "brotli",
            "has_threads": False,
            "comp": lambda i, a: (["brotli", "-q", "11", "--lgwin=24", "-f", "-o", a, i], None),
            "decomp": lambda a, o: (["brotli", "-d", "-f", "-o", o, a], None),
        },
        "bzip3-b32": {
            "binary": "bzip3",
            "has_threads": True,
            "comp": lambda i, a: (["bzip3", "-e", "-c", "-b", "32", "-j", th_s, i], a),
            "decomp": lambda a, o: (["bzip3", "-d", "-c", "-j", th_s, a], o),
        },
        "bzip3": {
            "binary": "bzip3",
            "has_threads": True,
            "comp": lambda i, a: (["bzip3", "-e", "-c", "-j", th_s, i], a),
            "decomp": lambda a, o: (["bzip3", "-d", "-c", "-j", th_s, a], o),
        },
        "bsc-b32": {
            "binary": bsc,
            "has_threads": True,
            "comp": lambda i, a: ([bsc, "e", i, a, "-b32"] + bsc_th, None),
            "decomp": lambda a, o: ([bsc, "d", a, o] + bsc_th, None),
        },
        "bsc-b32-e2": {
            "binary": bsc,
            "has_threads": True,
            "comp": lambda i, a: ([bsc, "e", i, a, "-b32", "-e2"] + bsc_th, None),
            "decomp": lambda a, o: ([bsc, "d", a, o] + bsc_th, None),
        },
        "bsc": {
            "binary": bsc,
            "has_threads": True,
            "comp": lambda i, a: ([bsc, "e", i, a] + bsc_th, None),
            "decomp": lambda a, o: ([bsc, "d", a, o] + bsc_th, None),
        },
        "kanzi-7": {
            "binary": "kanzi",
            "has_threads": True,
            "comp": lambda i, a: (["kanzi", "-c", "-i", i, "-o", a, "-l", "7", "-j", th_s, "-f", "-v", "0"], None),
            "decomp": lambda a, o: (["kanzi", "-d", "-i", a, "-o", o, "-j", th_s, "-f", "-v", "0"], None),
        },
        "kanzi-9": {
            "binary": "kanzi",
            "has_threads": True,
            "comp": lambda i, a: (["kanzi", "-c", "-i", i, "-o", a, "-l", "9", "-j", th_s, "-f", "-v", "0"], None),
            "decomp": lambda a, o: (["kanzi", "-d", "-i", a, "-o", o, "-j", th_s, "-f", "-v", "0"], None),
        },
        "zpaq-m5": {
            "binary": "zpaq",
            "has_threads": True,
            "comp": lambda i, a: (["zpaq", "a", a, i, "-m5", "-threads", th_s], None),
            "decomp": lambda a, o: (["zpaq", "x", a, "-to", o, "-force", "-threads", th_s], None),
        },
        "paq8px-8": {
            "binary": "paq8px",
            "has_threads": False,
            "max_file_size": 16 * 1024 * 1024,
            "comp": lambda i, a: (["paq8px", "-8", a, i], None),
            "decomp": lambda a, o: (["paq8px", "-d", a], None),
        },
    }


DEFAULT_BENCH_TOOLS = [
    "apixel",
    "apixel-nv",
    "7z-ultra",
    "xz-9",
    "xz-9e",
    "zstd-19",
    "zstd-22-long",
    "brotli-11",
    "bzip3-b32",
    "bsc-b32",
    "bsc-b32-e2",
    "kanzi-7",
    "kanzi-9",
    "zpaq-m5",
    "paq8px-8",
]

ALIASES = {
    "kanzi": ["kanzi-7", "kanzi-9"],
    "kanzi-l7": ["kanzi-7"],
    "kanzi-l9": ["kanzi-9"],
    "zpaq": ["zpaq-m5"],
    "zpaq-5": ["zpaq-m5"],
    "paq8px": ["paq8px-8"],
    "xz": ["xz-9", "xz-9e"],
    "zstd": ["zstd-19", "zstd-22-long"],
}


def find_actual_arc(arc, tmp):
    """Finds the actual archive file path if tool appended an extension."""
    if os.path.isfile(arc):
        return arc
    base = os.path.basename(arc)
    for f in os.listdir(tmp):
        p = os.path.join(tmp, f)
        if os.path.isfile(p) and f.startswith(base):
            return p
    return arc


def ensure_output_file(out, tmp, expected_basename, arc_path):
    """Ensures decompressed output file is placed at out path."""
    if os.path.isfile(out):
        return
    # If arc_path had an extra extension (like paq8px .paq8px217),
    # decompressing it produces arc.bin in tmp
    cand_arc_bin = os.path.join(tmp, "arc.bin")
    if arc_path != cand_arc_bin and os.path.isfile(cand_arc_bin):
        shutil.move(cand_arc_bin, out)
        return
    # If out was created as a directory (e.g. zpaq -to <dir>)
    if os.path.isdir(out):
        for root, dirs, files in os.walk(out):
            for f in files:
                src = os.path.join(root, f)
                tmp_mv = os.path.join(tmp, "__recovered_out__")
                shutil.move(src, tmp_mv)
                shutil.rmtree(out)
                shutil.move(tmp_mv, out)
                return
    # If extracted into tmp with the original basename
    cand = os.path.join(tmp, expected_basename)
    if os.path.isfile(cand) and cand != out:
        shutil.move(cand, out)
        return
    # Look for any newly created file that is not the archive
    for root, dirs, files in os.walk(tmp):
        for f in files:
            p = os.path.join(root, f)
            if p == arc_path:
                continue
            shutil.move(p, out)
            return


def parse_tools_arg(tools_arg, table):
    """Expands tool list including aliases."""
    if not tools_arg:
        return [t for t in DEFAULT_BENCH_TOOLS if t in table]
    res = []
    for item in tools_arg.split(","):
        t = item.strip()
        if not t:
            continue
        if t in ALIASES:
            for sub in ALIASES[t]:
                if sub not in res:
                    res.append(sub)
        elif t in table:
            if t not in res:
                res.append(t)
        else:
            # Pass unknown tool so caller can report not found
            if t not in res:
                res.append(t)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apixel", default="build/apixel")
    ap.add_argument("--bsc", default="bsc")
    ap.add_argument("--threads", "-T", type=int, default=1, help="threads to use for all tools (default: 1)")
    ap.add_argument("--tool-dir", default=DEFAULT_TOOL_DIR, help="directory to search for tool binaries")
    ap.add_argument("--out", default="results.csv")
    ap.add_argument("--tools", default="")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    table = tools(args.apixel, args.bsc, args.threads)
    names = parse_tools_arg(args.tools, table)
    tmp = tempfile.mkdtemp(prefix="apxbench-")
    rows = []

    try:
        for name in names:
            if name not in table:
                print(f"skip {name}: unknown tool", flush=True)
                continue
            tool_entry = table[name]
            binary_req = tool_entry["binary"]
            bin_path = resolve_binary(binary_req, args.tool_dir)
            if not bin_path:
                print(f"skip {name}: '{binary_req}' not found", flush=True)
                continue

            tool_ver = query_tool_version(bin_path)
            threads_str = str(args.threads) if tool_entry["has_threads"] else "1 (no mt)"

            for path in args.files:
                raw = os.path.getsize(path)
                max_size = tool_entry.get("max_file_size")
                if max_size and raw >= max_size:
                    print(f"skip {name} on {os.path.basename(path)}: size {raw} >= {max_size // 1048576} MiB limit", flush=True)
                    continue

                arc, out = os.path.join(tmp, "arc.bin"), os.path.join(tmp, "out")
                # Clean up previous iteration outputs in tmp
                for p in list(os.listdir(tmp)):
                    fp = os.path.join(tmp, p)
                    if os.path.isdir(fp):
                        shutil.rmtree(fp, ignore_errors=True)
                    elif os.path.isfile(fp):
                        try:
                            os.remove(fp)
                        except OSError:
                            pass

                cmd, so = tool_entry["comp"](path, arc)
                cmd[0] = bin_path
                ct, cm = run(cmd, so)

                actual_arc = find_actual_arc(arc, tmp)
                size = os.path.getsize(actual_arc)

                cmd, so = tool_entry["decomp"](actual_arc, out)
                cmd[0] = bin_path
                dt, dm = run(cmd, so)

                ensure_output_file(out, tmp, os.path.basename(path), actual_arc)
                ok = os.path.isfile(out) and (sha256(out) == sha256(path))

                c_spd = (raw / 1048576.0 / ct) if ct > 0 else 0.0
                d_spd = (raw / 1048576.0 / dt) if dt > 0 else 0.0

                rows.append(dict(
                    tool=name,
                    version=tool_ver,
                    threads=threads_str,
                    file=os.path.basename(path),
                    raw=raw,
                    size=size,
                    c_sec=round(ct, 3),
                    d_sec=round(dt, 3),
                    c_mib=round(cm),
                    d_mib=round(dm),
                    ok=ok,
                ))

                print(f"{name:13s} {os.path.basename(path):10s} {raw:>10d} -> {size:>10d}  "
                      f"c {c_spd:6.2f} MiB/s  d {d_spd:7.2f} MiB/s  "
                      f"mem {cm:.0f}/{dm:.0f} MiB  {'ok' if ok else 'MISMATCH'}", flush=True)
                if not ok:
                    raise RuntimeError(f"{name}: round-trip mismatch on {path}")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        if rows:
            with open(args.out, "w", newline="") as f:
                w = csv.DictWriter(f, fieldnames=list(rows[0]))
                w.writeheader()
                w.writerows(rows)
            print("wrote", args.out, flush=True)


if __name__ == "__main__":
    main()
