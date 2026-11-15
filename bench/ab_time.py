#!/usr/bin/env python3
"""Parallel interleaved A/B timing of apixel binaries, one file per physical core.

usage: bench/ab_time.py [--runs N] [--slots K] NAME=binary[::extra args] ... -- FILES...
  e.g. bench/ab_time.py base=build-base/apixel new=build/apixel -- corpus/silesia/*
       bench/ab_time.py r1=a/apixel::-m lzb r2=b/apixel::-m lzb -- corpus/silesia/mozilla

Each file takes a free core slot (the same /tmp/apixel-core-<k>.lock slots as bench/corelock.sh, shared hold of
/tmp/apixel-bench.lock), so up to K files are timed at once while builds and agents keep running. On its core the
variants are run interleaved, N rounds, single thread, compress (--no-verify) and decompress; the minimum is kept.
The first round of every variant checks the round trip (cmp). The ratio column is relative to the first variant.
Archive sizes are printed too, and per-variant totals at the end.
"""
import argparse, concurrent.futures as cf, fcntl, filecmp, os, subprocess, sys, tempfile, threading, time

CORES = (1, 2, 3, 4, 5)


def parse():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--slots", type=int, default=len(CORES))
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    if "--" not in a.rest:
        ap.error("need NAME=binary ... -- FILES")
    i = a.rest.index("--")
    variants = []
    for spec in a.rest[:i]:
        name, _, rhs = spec.partition("=")
        binary, _, extra = rhs.partition("::")
        variants.append((name, os.path.abspath(binary), extra.split()))
    files = [os.path.abspath(f) for f in a.rest[i + 1:] if os.path.isfile(f)]
    if not variants or not files:
        ap.error("no variants or no files")
    return a.runs, min(a.slots, len(CORES)), variants, files


def acquire_core(stop):
    while not stop.is_set():
        for k in CORES:
            fd = os.open(f"/tmp/apixel-core-{k}.lock", os.O_CREAT | os.O_RDWR, 0o666)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return k, fd
            except BlockingIOError:
                os.close(fd)
        time.sleep(0.3)
    raise SystemExit(1)


def timed(core, cmd):
    t0 = time.perf_counter()
    subprocess.run(["taskset", "-c", str(core), *cmd], check=True, stdout=subprocess.DEVNULL)
    return time.perf_counter() - t0


def one_file(path, runs, variants, tmp, stop):
    core, fd = acquire_core(stop)
    try:
        d = tempfile.mkdtemp(dir=tmp)
        best = {n: [1e9, 1e9, 0] for n, _, _ in variants}
        for r in range(runs):
            for n, b, extra in variants:
                arc, out = os.path.join(d, n + ".apx"), os.path.join(d, n + ".out")
                best[n][0] = min(best[n][0], timed(core, [b, "c", "-T", "1", "--no-verify", "-q", "-f", *extra, "-o", arc, path]))
                best[n][1] = min(best[n][1], timed(core, [b, "x", "-T", "1", "-q", "-f", "-o", out, arc]))
                if r == 0:
                    best[n][2] = os.path.getsize(arc)
                    if not filecmp.cmp(path, out, shallow=False):
                        raise RuntimeError(f"ROUND-TRIP MISMATCH: {n} on {path}")
                os.remove(out)
        return path, core, best
    finally:
        os.close(fd)


def main():
    runs, slots, variants, files = parse()
    lock = os.open(os.environ.get("APX_BENCH_LOCK", "/tmp/apixel-bench.lock"), os.O_CREAT | os.O_RDWR, 0o666)
    fcntl.flock(lock, fcntl.LOCK_SH)  # waits while a whole-machine run holds it exclusively
    base = variants[0][0]
    tot = {n: [0, 0.0, 0.0] for n, _, _ in variants}
    raw_total = 0
    stop = threading.Event()
    with tempfile.TemporaryDirectory(prefix="apx_ab_") as tmp, cf.ThreadPoolExecutor(slots) as ex:
        futs = [ex.submit(one_file, f, runs, variants, tmp, stop) for f in files]
        try:
            for fu in cf.as_completed(futs):
                path, core, best = fu.result()
                mib = os.path.getsize(path) / 2**20
                raw_total += os.path.getsize(path)
                b = best[base]
                for n, (c, x, sz) in best.items():
                    tot[n][0] += sz; tot[n][1] += c; tot[n][2] += x
                    print(f"{os.path.basename(path):10} core{core} {n:10} {sz:11d}  c {mib / c:7.2f} MiB/s ×{b[0] / c:.3f}"
                          f"  d {mib / x:7.2f} MiB/s ×{b[1] / x:.3f}", flush=True)
        except BaseException:
            stop.set()
            raise
    mib = raw_total / 2**20
    b = tot[base]
    for n, (sz, c, x) in tot.items():
        print(f"TOTAL {n:10} {sz:11d} ({100 * (sz / b[0] - 1):+.3f}%)  c {mib / c:7.2f} MiB/s ×{b[1] / c:.3f}"
              f"  d {mib / x:7.2f} MiB/s ×{b[2] / x:.3f}")


if __name__ == "__main__":
    sys.exit(main())
