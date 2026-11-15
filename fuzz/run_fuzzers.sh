#!/bin/bash
# run_fuzzers.sh [seconds-per-target] [targets...] — runs libFuzzer targets ONE AT A TIME, each on all cores
# (libFuzzer -fork=$JOBS, default nproc), so every target finishes fast. Default 600 s per target.
# fuzz_cm1_decode_big (1 MiB output cap, slow) runs only when named explicitly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
# libFuzzer -fork builds child command lines without quoting, so a path with spaces makes every child fail
# (exit 127, 0 exec/s). Work through a space-free symlink in that case.
if [[ "$ROOT_DIR" == *" "* ]]; then
    link="${TMPDIR:-/tmp}/apixel-fuzz-$(id -u)-$(printf %s "$ROOT_DIR" | cksum | cut -d" " -f1)"
    ln -sfn "$ROOT_DIR" "$link"
    ROOT_DIR="$link"
    SCRIPT_DIR="$link/fuzz"
fi
BUILD_DIR="${ROOT_DIR}/build-fuzz"

TARGETS=(
    fuzz_cm1_decode
    fuzz_arm64_decode
    fuzz_bwt_decode
    fuzz_x86_decode
    fuzz_decompress_stream
    fuzz_libsais_unbwt
)

TIME="${1:-600}"
shift || true
[ $# -gt 0 ] && TARGETS=("$@")
JOBS="${JOBS:-$(nproc)}"

echo "Fuzzing ${#TARGETS[@]} targets one after another, ${TIME}s each, ${JOBS} parallel jobs per target..."
for t in "${TARGETS[@]}"; do
    mkdir -p "${SCRIPT_DIR}/corpus/${t}" "${SCRIPT_DIR}/crashes/${t}"
    seeds="${SCRIPT_DIR}/seeds/${t%_big}"
    log_file="${SCRIPT_DIR}/log_${t}.txt"
    echo "--- ${t} -> ${log_file}"
    "${BUILD_DIR}/fuzz/${t}" "${SCRIPT_DIR}/corpus/${t}" "${seeds}" \
        -fork="${JOBS}" -ignore_crashes=1 -max_total_time="${TIME}" \
        -artifact_prefix="${SCRIPT_DIR}/crashes/${t}/" > "${log_file}" 2>&1 || true
    tail -n 2 "${log_file}"
    n=$(find "${SCRIPT_DIR}/crashes/${t}" -type f | wc -l)
    echo "    crashes: ${n}"
done
