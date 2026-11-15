#!/bin/sh
# End-to-end test of the command-line tool. Usage: cli_test.sh <path-to-apixel>
set -eu
APX="$1"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT
fail() { echo "FAIL: $1" >&2; exit 1; }

# Inputs: text-like, binary (the program itself), random, empty.
i=0; while [ $i -lt 4000 ]; do echo "line $i: the quick brown fox jumps over the lazy dog $((i * i % 977))"; i=$((i + 1)); done > "$DIR/text"
cp "$APX" "$DIR/binary"
head -c 300000 /dev/urandom > "$DIR/random"
: > "$DIR/empty"

for f in text binary random empty; do
  "$APX" c -q -b 1 "$DIR/$f"                         || fail "compress $f"
  "$APX" t -q "$DIR/$f.apx"                          || fail "test $f"
  "$APX" x -q "$DIR/$f.apx" -o "$DIR/$f.out"         || fail "extract $f"
  cmp -s "$DIR/$f" "$DIR/$f.out"                     || fail "round-trip mismatch on $f"
  "$APX" l "$DIR/$f.apx" > /dev/null                 || fail "list $f"
done

# Same bytes regardless of thread count; stdin/stdout work.
"$APX" c -q -b 1 -T 1 "$DIR/binary" -o "$DIR/b1.apx"
"$APX" c -q -b 1 -T 4 "$DIR/binary" -o "$DIR/b4.apx"
cmp -s "$DIR/b1.apx" "$DIR/b4.apx"                   || fail "output depends on thread count"
"$APX" c -q - < "$DIR/text" | "$APX" x -q - > "$DIR/text.pipe"
cmp -s "$DIR/text" "$DIR/text.pipe"                  || fail "pipe round-trip"

# Random data must be stored: overhead of a few dozen bytes at most.
RAW=$(wc -c < "$DIR/random"); ARC=$(wc -c < "$DIR/random.apx")
[ $((ARC - RAW)) -le 64 ]                            || fail "random data expanded by $((ARC - RAW)) bytes"

# Safety behaviour: no overwrite without -f; damaged archive -> error, no output file.
if "$APX" c -q "$DIR/text" 2>/dev/null; then fail "overwrote existing archive without -f"; fi
head -c 2000 "$DIR/text.apx" > "$DIR/trunc.apx"
if "$APX" x -q "$DIR/trunc.apx" 2>/dev/null; then fail "accepted a truncated archive"; fi
[ ! -e "$DIR/trunc" ] && [ ! -e "$DIR/trunc.tmp" ]   || fail "left output behind after a failed extract"

echo "cli tests: OK"
