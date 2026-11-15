#!/usr/bin/env bash
# bench/eval_model.sh — Model evaluator for CM1 experiments (Task T5.1).
#
# Evaluates stage_cm1 on BWT images of the tuning and holdout corpus sets.
# Compiles build-eval (Release, -DAPIXEL_TUNE=ON) in the specified worktree,
# runs build-eval/devtool cm1 on cached BWT images, and prints a single CSV row:
#   label,vars,size_tune,size_holdout,decisions,sec_tune,sec_holdout
#
# BWT Cache policy:
#   BWT images are cached in $CORPUS_DIR/bwt/<name>.bwt (default: corpus/ of the main checkout next to this script).
#   For corpus files larger than 32 MiB (33,554,432 bytes), the BWT image is
#   created from the first 32 MiB of the file, matching Apixel's default block size.
#
# Datasets:
#   Tune set (8 files):
#     dickens ooffice osdb reymont sao x-ray xml mr (Silesia)
#   Holdout set (5 files):
#     enwik8 mozilla nci samba webster (enwik8 + Silesia)
#
# Timing:
#   Timing is only measured when the --time flag is passed.
#   With --time, each evaluation run is executed 3 times via bench/corelock.sh (own physical core,
#   up to 5 timings in parallel; waits while a whole-machine run holds the lock exclusively),
#   and the minimum elapsed wall-clock time (in seconds) is reported.
#   Without --time, timing columns are left empty.
#
# Usage:
#   bench/eval_model.sh [--time] [--header] [--label=<name>] <worktree-path> [VAR=val ...]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCK_FILE="/tmp/apixel-bench.lock"
BLOCK_LIMIT=$((32 * 1024 * 1024))  # 32 MiB default block size

CORPUS_DIR="${CORPUS_DIR:-$(cd "$(dirname "$0")/.." && pwd)/corpus}"
BWT_DIR="${CORPUS_BWT_DIR:-$CORPUS_DIR/bwt}"

TUNE_NAMES=(dickens ooffice osdb reymont sao x-ray xml mr)
HOLDOUT_NAMES=(enwik8 mozilla nci samba webster)

MEASURE_TIME=0
PRINT_HEADER=0
LABEL_OVERRIDE=""
WORKTREE=""
VARS_LIST=()

usage() {
    cat << 'EOF' >&2
Usage: bench/eval_model.sh [OPTIONS] <worktree-path> [VAR=val ...]

Options:
  --time            Measure timing (minimum of 3 runs under /tmp/apixel-bench.lock)
  --header          Print CSV header before data row
  --label=<name>    Override label in output CSV (default: git branch or dir name)
  -h, --help        Show this help message

Datasets:
  Tune set:    dickens ooffice osdb reymont sao x-ray xml mr
  Holdout set: enwik8 mozilla nci samba webster
EOF
    exit "${1:-2}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --time)
            MEASURE_TIME=1
            shift
            ;;
        --header)
            PRINT_HEADER=1
            shift
            ;;
        --label=*)
            LABEL_OVERRIDE="${1#*=}"
            shift
            ;;
        --label)
            if [[ $# -lt 2 ]]; then
                echo "Error: --label requires an argument" >&2
                exit 2
            fi
            LABEL_OVERRIDE="$2"
            shift 2
            ;;
        -h|--help)
            usage 0
            ;;
        *=*)
            VARS_LIST+=("$1")
            shift
            ;;
        *)
            if [[ -z "$WORKTREE" ]]; then
                WORKTREE="$1"
            else
                echo "Error: Unexpected argument: $1" >&2
                usage
            fi
            shift
            ;;
    esac
done

if [[ -z "$WORKTREE" ]]; then
    echo "Error: Missing worktree path" >&2
    usage
fi

if [[ ! -d "$WORKTREE" ]]; then
    echo "Error: Worktree directory does not exist: $WORKTREE" >&2
    exit 1
fi

WORKTREE="$(cd "$WORKTREE" && pwd)"

if [[ ! -f "$WORKTREE/CMakeLists.txt" ]]; then
    echo "Error: Not a valid Apixel repository (no CMakeLists.txt): $WORKTREE" >&2
    exit 1
fi

# 1. Determine Label
if [[ -n "$LABEL_OVERRIDE" ]]; then
    LABEL="$LABEL_OVERRIDE"
elif git -C "$WORKTREE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    BRANCH=$(git -C "$WORKTREE" rev-parse --abbrev-ref HEAD 2>/dev/null || true)
    if [[ "$BRANCH" != "HEAD" && -n "$BRANCH" ]]; then
        LABEL="$BRANCH"
    else
        LABEL=$(git -C "$WORKTREE" rev-parse --short HEAD 2>/dev/null || basename "$WORKTREE")
    fi
else
    LABEL=$(basename "$WORKTREE")
fi

# 2. Build build-eval with APIXEL_TUNE=ON
BUILD_DIR="$WORKTREE/build-eval"
BUILD_LOG="$BUILD_DIR/build.log"
mkdir -p "$BUILD_DIR"

if ! cmake -S "$WORKTREE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DAPIXEL_TUNE=ON > "$BUILD_LOG" 2>&1; then
    cat "$BUILD_LOG" >&2
    echo "Error: CMake configuration failed in $WORKTREE" >&2
    exit 1
fi

NPROC=$(nproc 2>/dev/null || echo 4)
if ! cmake --build "$BUILD_DIR" --target devtool -j"$NPROC" >> "$BUILD_LOG" 2>&1; then
    cat "$BUILD_LOG" >&2
    echo "Error: Build of devtool failed in $WORKTREE" >&2
    exit 1
fi

DEVTOOL="$BUILD_DIR/devtool"
if [[ ! -x "$DEVTOOL" ]]; then
    echo "Error: Built devtool executable not found at $DEVTOOL" >&2
    exit 1
fi

# 3. Ensure BWT images exist in cache
mkdir -p "$BWT_DIR"

ensure_bwt() {
    local name="$1"
    local target="$BWT_DIR/${name}.bwt"
    if [[ -f "$target" ]]; then
        return 0
    fi

    local src=""
    if [[ "$name" == "enwik8" ]]; then
        src="$CORPUS_DIR/enwik8"
    else
        src="$CORPUS_DIR/silesia/$name"
    fi

    if [[ ! -f "$src" ]]; then
        echo "Error: Source corpus file not found: $src" >&2
        exit 1
    fi

    local size
    size=$(stat -c "%s" "$src" 2>/dev/null || wc -c < "$src")

    local tmp_target
    tmp_target=$(mktemp "$BWT_DIR/.${name}.bwt.tmp.XXXXXX")

    if (( size > BLOCK_LIMIT )); then
        local tmp_head
        tmp_head=$(mktemp "/tmp/apixel_head32_${name}_XXXXXX")
        head -c "$BLOCK_LIMIT" "$src" > "$tmp_head"
        if ! "$DEVTOOL" bwt "$tmp_head" "$tmp_target" >/dev/null 2>&1; then
            rm -f "$tmp_head" "$tmp_target"
            echo "Error: Failed to generate BWT for $name" >&2
            exit 1
        fi
        rm -f "$tmp_head"
    else
        if ! "$DEVTOOL" bwt "$src" "$tmp_target" >/dev/null 2>&1; then
            rm -f "$tmp_target"
            echo "Error: Failed to generate BWT for $name" >&2
            exit 1
        fi
    fi

    mv "$tmp_target" "$target"
}

for name in "${TUNE_NAMES[@]}" "${HOLDOUT_NAMES[@]}"; do
    ensure_bwt "$name"
done

TUNE_BWT_FILES=()
for name in "${TUNE_NAMES[@]}"; do
    TUNE_BWT_FILES+=("$BWT_DIR/${name}.bwt")
done

HOLDOUT_BWT_FILES=()
for name in "${HOLDOUT_NAMES[@]}"; do
    HOLDOUT_BWT_FILES+=("$BWT_DIR/${name}.bwt")
done

# 4. Helper for parsing devtool output
parse_total_size() {
    sed -n 's/.*TOTAL [0-9]\+ -> \([0-9]\+\).*/\1/p' <<< "$1"
}

parse_decisions() {
    awk '/decisions/ {for (i=1; i<=NF; i++) if ($i == "decisions") sum += $(i-1)} END {if (sum > 0) print sum}' <<< "$1"
}

# 5. Run evaluation
SIZE_TUNE=""
SIZE_HOLDOUT=""
DEC_TUNE=""
DEC_HOLDOUT=""
SEC_TUNE=""
SEC_HOLDOUT=""

if (( MEASURE_TIME == 1 )); then
    TMP_PIPE_DIR=$(mktemp -d "/tmp/apixel_eval_XXXXXX")
    trap 'rm -rf "$TMP_PIPE_DIR"' EXIT

    TIMES_TUNE=()
    OUT_TUNE=""
    for iter in 1 2 3; do
        (
            APX_TIME_FILE="$TMP_PIPE_DIR/tune_${iter}.time" "$SCRIPT_DIR/corelock.sh" env "${VARS_LIST[@]}" "$DEVTOOL" cm1 "${TUNE_BWT_FILES[@]}" > "$TMP_PIPE_DIR/tune_${iter}.out"
        ) 200>"$LOCK_FILE"
        dt=$(cat "$TMP_PIPE_DIR/tune_${iter}.time")
        TIMES_TUNE+=("$dt")
        OUT_TUNE=$(cat "$TMP_PIPE_DIR/tune_${iter}.out")
    done
    SEC_TUNE=$(python3 -c "import sys; print(f'{min(float(x) for x in sys.argv[1:]):.3f}')" "${TIMES_TUNE[@]}")
    SIZE_TUNE=$(parse_total_size "$OUT_TUNE")
    DEC_TUNE=$(parse_decisions "$OUT_TUNE")

    TIMES_HOLDOUT=()
    OUT_HOLDOUT=""
    for iter in 1 2 3; do
        (
            APX_TIME_FILE="$TMP_PIPE_DIR/holdout_${iter}.time" "$SCRIPT_DIR/corelock.sh" env "${VARS_LIST[@]}" "$DEVTOOL" cm1 "${HOLDOUT_BWT_FILES[@]}" > "$TMP_PIPE_DIR/holdout_${iter}.out"
        ) 200>"$LOCK_FILE"
        dt=$(cat "$TMP_PIPE_DIR/holdout_${iter}.time")
        TIMES_HOLDOUT+=("$dt")
        OUT_HOLDOUT=$(cat "$TMP_PIPE_DIR/holdout_${iter}.out")
    done
    SEC_HOLDOUT=$(python3 -c "import sys; print(f'{min(float(x) for x in sys.argv[1:]):.3f}')" "${TIMES_HOLDOUT[@]}")
    SIZE_HOLDOUT=$(parse_total_size "$OUT_HOLDOUT")
    DEC_HOLDOUT=$(parse_decisions "$OUT_HOLDOUT")
else
    OUT_TUNE=$(env "${VARS_LIST[@]}" "$DEVTOOL" cm1 "${TUNE_BWT_FILES[@]}")
    SIZE_TUNE=$(parse_total_size "$OUT_TUNE")
    DEC_TUNE=$(parse_decisions "$OUT_TUNE")

    OUT_HOLDOUT=$(env "${VARS_LIST[@]}" "$DEVTOOL" cm1 "${HOLDOUT_BWT_FILES[@]}")
    SIZE_HOLDOUT=$(parse_total_size "$OUT_HOLDOUT")
    DEC_HOLDOUT=$(parse_decisions "$OUT_HOLDOUT")
fi

TOTAL_DECISIONS=""
if [[ -n "$DEC_TUNE" || -n "$DEC_HOLDOUT" ]]; then
    TOTAL_DECISIONS=$(( ${DEC_TUNE:-0} + ${DEC_HOLDOUT:-0} ))
fi

# 6. Format CSV Row
format_csv_field() {
    local val="$1"
    if [[ "$val" =~ [,\"] || "$val" =~ [[:space:]] ]]; then
        val="${val//\"/\"\"}"
        printf '"%s"' "$val"
    else
        printf '%s' "$val"
    fi
}

VARS_STRING=""
if [[ ${#VARS_LIST[@]} -gt 0 ]]; then
    VARS_STRING="${VARS_LIST[*]}"
fi

CSV_LABEL=$(format_csv_field "$LABEL")
CSV_VARS=$(format_csv_field "$VARS_STRING")

if (( PRINT_HEADER == 1 )); then
    echo "label,vars,size_tune,size_holdout,decisions,sec_tune,sec_holdout"
fi

echo "${CSV_LABEL},${CSV_VARS},${SIZE_TUNE},${SIZE_HOLDOUT},${TOTAL_DECISIONS},${SEC_TUNE},${SEC_HOLDOUT}"
