#!/bin/bash
# make_seeds.sh: Generates seed corpora for libFuzzer targets using apixel and seed_gen.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${ROOT_DIR}/build-fuzz}"
APX="${BUILD_DIR}/apixel"
SEED_GEN="${BUILD_DIR}/fuzz/seed_gen"

if [ ! -x "${APX}" ]; then
    # Fallback to build-g-t4 if build-fuzz apixel is not yet compiled
    if [ -x "${ROOT_DIR}/build-g-t4/apixel" ]; then
        APX="${ROOT_DIR}/build-g-t4/apixel"
    else
        echo "Error: apixel executable not found at ${APX} or ${ROOT_DIR}/build-g-t4/apixel" >&2
        exit 1
    fi
fi

SEEDS_DIR="${SCRIPT_DIR}/seeds"
CRASHES_DIR="${SCRIPT_DIR}/crashes"

mkdir -p "${SEEDS_DIR}/fuzz_decompress_stream"
mkdir -p "${SEEDS_DIR}/fuzz_bwt_decode"
mkdir -p "${SEEDS_DIR}/fuzz_cm1_decode"
mkdir -p "${SEEDS_DIR}/fuzz_x86_decode"
mkdir -p "${SEEDS_DIR}/fuzz_arm64_decode"
mkdir -p "${SEEDS_DIR}/fuzz_libsais_unbwt"

mkdir -p "${CRASHES_DIR}/fuzz_decompress_stream"
mkdir -p "${CRASHES_DIR}/fuzz_bwt_decode"
mkdir -p "${CRASHES_DIR}/fuzz_cm1_decode"
mkdir -p "${CRASHES_DIR}/fuzz_x86_decode"
mkdir -p "${CRASHES_DIR}/fuzz_arm64_decode"
mkdir -p "${CRASHES_DIR}/fuzz_libsais_unbwt"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "Generating archive seeds using ${APX}..."

# 1. Empty input
: > "${TMP_DIR}/empty"
"${APX}" c -q -f -b 1 "${TMP_DIR}/empty" -o "${SEEDS_DIR}/fuzz_decompress_stream/seed_empty.apx"

# 2. Text input
cat << 'EOF' > "${TMP_DIR}/text"
The quick brown fox jumps over the lazy dog.
Apixel v2 is an experimental high-ratio data compressor written in C++20.
It uses block-level Burrows-Wheeler transform combined with CM1 run and rank context mixing.
EOF
"${APX}" c -q -f -b 1 "${TMP_DIR}/text" -o "${SEEDS_DIR}/fuzz_decompress_stream/seed_text.apx"

# 3. Repetitive patterns / runs
python3 -c "import sys; sys.stdout.buffer.write(b'A' * 200 + b'B' * 300 + b'CD' * 150)" > "${TMP_DIR}/runs"
"${APX}" c -q -f -b 1 "${TMP_DIR}/runs" -o "${SEEDS_DIR}/fuzz_decompress_stream/seed_runs.apx"

# 4. Small binary input with x86 instructions
head -c 1024 "${APX}" > "${TMP_DIR}/binary"
"${APX}" c -q -f -b 1 "${TMP_DIR}/binary" -o "${SEEDS_DIR}/fuzz_decompress_stream/seed_binary.apx"

# 5. Pseudo-random input (tests stored block framing)
python3 -c "import sys, random; random.seed(42); sys.stdout.buffer.write(bytes(random.randint(0, 255) for _ in range(512)))" > "${TMP_DIR}/random"
"${APX}" c -q -f -b 1 "${TMP_DIR}/random" -o "${SEEDS_DIR}/fuzz_decompress_stream/seed_random.apx"

# Generate stage seeds if seed_gen tool is available
if [ -x "${SEED_GEN}" ]; then
    echo "Generating stage decoder seeds using ${SEED_GEN}..."
    "${SEED_GEN}" "${SEEDS_DIR}"
fi

echo "Seeds successfully created in ${SEEDS_DIR}:"
find "${SEEDS_DIR}" -type f | sort
