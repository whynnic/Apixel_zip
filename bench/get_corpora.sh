#!/bin/sh
# bench/get_corpora.sh — download and verify benchmark corpora for Apixel (POSIX sh).
#
# Sources:
#   - Silesia: https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
#   - enwik8:  https://mattmahoney.net/dc/enwik8.zip
#
# Directory layout:
#   corpus/dl/*.zip
#   corpus/silesia/<12 files>
#   corpus/enwik8
#
# Tools used: only curl, unzip, sha256sum (and standard POSIX shell utilities).

# Reference SHA-256 sums matching corpus/SHA256SUMS (paths relative to corpus/).
checksums_data() {
cat <<'EOF'
547994d9980ebed1288380d652999f38a14fe291a6247c157c3d33d4932534bc  dl/enwik8.zip
0626e25f45c0ffb5dc801f13b7c82a3b75743ba07e3a71835a41e3d9f63c77af  dl/silesia.zip
b24c37886142e11d0ee687db6ab06f936207aa7f2ea1fd1d9a36763c7a507e6a  silesia/dickens
657fc3764b0c75ac9de9623125705831ebbfbe08fed248df73bc2dc66e2a963b  silesia/mozilla
68637ed52e3e4860174ed2dc0840ac77d5f1a60abbcb13770d5754e3774d53e6  silesia/mr
fc63a31770947b8c2062d3b19ca94c00485a232bb91b502021948fee983e1635  silesia/nci
e7ee013880d34dd5208283d0d3d91b07f442e067454276095ded14f322a656eb  silesia/ooffice
60f027179302ca3ad87c58ac90b6be72ec23588aaa7a3b7fe8ecc0f11def3fa3  silesia/osdb
0eac0114a3dfe6e2ee1f345a0f79d653cb26c3bc9f0ed79238af4933422b7578  silesia/reymont
93ba07bc44d8267789c1d911992f40b089ffa2140b4a160fac11ccae9a40e7b2  silesia/samba
c2d0ea2cc59d4c21b7fe43a71499342a00cbe530a1d5548770e91ecd6214adcc  silesia/sao
6a68f69b26daf09f9dd84f7470368553194a0b294fcfa80f1604efb11143a383  silesia/webster
0e82e54e695c1938e4193448022543845b33020c8be6bf3bf3ead2224903e08c  silesia/xml
7de9fce1405dc44ae5e6813ed21cd5751e761bd4265655a005d39b9685d1c9ad  silesia/x-ray
2b49720ec4d78c3c9fabaee6e4179a5e997302b3a70029f30f2d582218c024a8  enwik8
EOF
}

# Computes 64-char hex SHA-256 using sha256sum and shell builtins (no awk/cut).
compute_sha256() {
    sha256sum "$1" 2>/dev/null | { read -r _hash _rest && printf "%s\n" "$_hash"; }
}

# Looks up expected SHA-256 checksum by exact relative path or by filename.
get_expected() {
    _target="$1"
    checksums_data | while read -r _sha _rel; do
        if [ "$_rel" = "$_target" ] || [ "${_rel##*/}" = "$_target" ]; then
            printf "%s\n" "$_sha"
            break
        fi
    done
}

# Verifies a single file against expected checksum.
verify_single_file() {
    _file="$1"
    _fname="${_file##*/}"
    _expected="$(get_expected "$_fname")"
    if [ -z "$_expected" ]; then
        printf "ERROR: Unknown corpus file: %s\n" "$_file" >&2
        return 1
    fi

    _actual="$(compute_sha256 "$_file")"
    if [ "$_actual" = "$_expected" ]; then
        printf "%s: OK\n" "$_file"
        return 0
    else
        printf "ERROR: %s: checksum mismatch\n" "$_file" >&2
        printf "  Expected: %s\n" "$_expected" >&2
        printf "  Actual:   %s\n" "$_actual" >&2
        return 1
    fi
}

# Verifies all 15 corpus files in the specified directory.
verify_directory() {
    _dir="$1"
    _errors=0
    _checked=0

    while read -r _expected _rel; do
        [ -z "$_expected" ] && continue
        _checked=$((_checked + 1))

        # Check standard hierarchical path first, then flat fallback.
        if [ -f "$_dir/$_rel" ]; then
            _fpath="$_dir/$_rel"
        elif [ -f "$_dir/${_rel##*/}" ]; then
            _fpath="$_dir/${_rel##*/}"
        else
            printf "%s: MISSING (file not found in %s)\n" "$_rel" "$_dir" >&2
            _errors=$((_errors + 1))
            continue
        fi

        _actual="$(compute_sha256 "$_fpath")"
        if [ "$_actual" = "$_expected" ]; then
            printf "%s: OK\n" "$_rel"
        else
            printf "ERROR: %s: checksum mismatch\n" "$_rel" >&2
            printf "  Expected: %s\n" "$_expected" >&2
            printf "  Actual:   %s\n" "$_actual" >&2
            _errors=$((_errors + 1))
        fi
    done <<EOF
$(checksums_data)
EOF

    if [ "$_errors" -ne 0 ]; then
        printf "Verification FAILED: %d error(s) out of %d files.\n" "$_errors" "$_checked" >&2
        return 1
    fi
    printf "Verification OK: all %d corpus files verified successfully.\n" "$_checked"
    return 0
}

show_help() {
    cat <<EOF
Usage: $0 [OPTIONS] [TARGET_DIR]

Downloads Silesia and enwik8 corpora into TARGET_DIR and verifies SHA-256 sums.
Default TARGET_DIR is "corpus/" adjacent to the repository.

Options:
  --verify-only, -v    Verify checksums without downloading or extracting.
                       TARGET_DIR can be a corpus directory or a single corpus file.
  --corpus-dir DIR     Specify target corpus directory.
  -d DIR               Short alias for --corpus-dir.
  -h, --help           Show this help message.

Sources:
  - Silesia: https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
  - enwik8:  https://mattmahoney.net/dc/enwik8.zip
EOF
}

# Parse command line options.
VERIFY_ONLY=0
CORPUS_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --verify-only|-v)
            VERIFY_ONLY=1
            shift
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        --corpus-dir)
            if [ -z "$2" ]; then
                printf "ERROR: --corpus-dir requires a directory path\n" >&2
                exit 1
            fi
            CORPUS_DIR="$2"
            shift 2
            ;;
        --corpus-dir=*)
            CORPUS_DIR="${1#*=}"
            shift
            ;;
        -d)
            if [ -z "$2" ]; then
                printf "ERROR: -d requires a directory path\n" >&2
                exit 1
            fi
            CORPUS_DIR="$2"
            shift 2
            ;;
        -*)
            printf "ERROR: Unknown option: %s\n" "$1" >&2
            show_help >&2
            exit 1
            ;;
        *)
            if [ -z "$CORPUS_DIR" ]; then
                CORPUS_DIR="$1"
            else
                printf "ERROR: Unexpected argument: %s\n" "$1" >&2
                show_help >&2
                exit 1
            fi
            shift
            ;;
    esac
done

# If no target specified, use default: corpus/ adjacent to repository.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -z "$CORPUS_DIR" ]; then
    if [ -d "$REPO_DIR/../corpus" ]; then
        CORPUS_DIR="$REPO_DIR/../corpus"
    elif [ -d "$REPO_DIR/corpus" ]; then
        CORPUS_DIR="$REPO_DIR/corpus"
    else
        CORPUS_DIR="$REPO_DIR/../corpus"
    fi
fi

# In --verify-only mode: verify single file or entire directory.
if [ "$VERIFY_ONLY" = "1" ]; then
    if [ -f "$CORPUS_DIR" ]; then
        verify_single_file "$CORPUS_DIR"
        exit $?
    elif [ -d "$CORPUS_DIR" ]; then
        verify_directory "$CORPUS_DIR"
        exit $?
    else
        printf "ERROR: Target '%s' does not exist.\n" "$CORPUS_DIR" >&2
        exit 1
    fi
fi

# Normal mode: download and extract.
SILESIA_URL="https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip"
ENWIK8_URL="https://mattmahoney.net/dc/enwik8.zip"
SILESIA_ZIP_SHA="0626e25f45c0ffb5dc801f13b7c82a3b75743ba07e3a71835a41e3d9f63c77af"
ENWIK8_ZIP_SHA="547994d9980ebed1288380d652999f38a14fe291a6247c157c3d33d4932534bc"
ENWIK8_RAW_SHA="2b49720ec4d78c3c9fabaee6e4179a5e997302b3a70029f30f2d582218c024a8"

mkdir -p "$CORPUS_DIR/dl" "$CORPUS_DIR/silesia"

# Clean up partial download files on interrupt.
trap 'rm -f "$CORPUS_DIR/dl"/*.part* 2>/dev/null; exit 1' INT TERM HUP

# 1. Check or download dl/silesia.zip.
SILESIA_ZIP="$CORPUS_DIR/dl/silesia.zip"
if [ -f "$SILESIA_ZIP" ]; then
    actual="$(compute_sha256 "$SILESIA_ZIP")"
    if [ "$actual" = "$SILESIA_ZIP_SHA" ]; then
        printf "dl/silesia.zip: OK (already downloaded)\n"
    else
        printf "ERROR: %s exists but checksum mismatch!\n" "$SILESIA_ZIP" >&2
        printf "  Expected: %s\n" "$SILESIA_ZIP_SHA" >&2
        printf "  Actual:   %s\n" "$actual" >&2
        exit 1
    fi
else
    printf "Downloading Silesia corpus (%s) ...\n" "$SILESIA_URL"
    part="$SILESIA_ZIP.part$$"
    if ! curl -f -L --retry 3 -o "$part" "$SILESIA_URL"; then
        rm -f "$part"
        printf "ERROR: Failed to download %s\n" "$SILESIA_URL" >&2
        exit 1
    fi
    actual="$(compute_sha256 "$part")"
    if [ "$actual" != "$SILESIA_ZIP_SHA" ]; then
        rm -f "$part"
        printf "ERROR: Downloaded dl/silesia.zip checksum mismatch!\n" >&2
        printf "  Expected: %s\n" "$SILESIA_ZIP_SHA" >&2
        printf "  Actual:   %s\n" "$actual" >&2
        exit 1
    fi
    mv "$part" "$SILESIA_ZIP"
    printf "dl/silesia.zip: OK\n"
fi

# 2. Check or download dl/enwik8.zip.
ENWIK8_ZIP="$CORPUS_DIR/dl/enwik8.zip"
if [ -f "$ENWIK8_ZIP" ]; then
    actual="$(compute_sha256 "$ENWIK8_ZIP")"
    if [ "$actual" = "$ENWIK8_ZIP_SHA" ]; then
        printf "dl/enwik8.zip: OK (already downloaded)\n"
    else
        printf "ERROR: %s exists but checksum mismatch!\n" "$ENWIK8_ZIP" >&2
        printf "  Expected: %s\n" "$ENWIK8_ZIP_SHA" >&2
        printf "  Actual:   %s\n" "$actual" >&2
        exit 1
    fi
else
    printf "Downloading enwik8 corpus (%s) ...\n" "$ENWIK8_URL"
    part="$ENWIK8_ZIP.part$$"
    if ! curl -f -L --retry 3 -o "$part" "$ENWIK8_URL"; then
        rm -f "$part"
        printf "ERROR: Failed to download %s\n" "$ENWIK8_URL" >&2
        exit 1
    fi
    actual="$(compute_sha256 "$part")"
    if [ "$actual" != "$ENWIK8_ZIP_SHA" ]; then
        rm -f "$part"
        printf "ERROR: Downloaded dl/enwik8.zip checksum mismatch!\n" >&2
        printf "  Expected: %s\n" "$ENWIK8_ZIP_SHA" >&2
        printf "  Actual:   %s\n" "$actual" >&2
        exit 1
    fi
    mv "$part" "$ENWIK8_ZIP"
    printf "dl/enwik8.zip: OK\n"
fi

# 3. Check or extract Silesia (12 files).
silesia_need_extract=0
for fname in dickens mozilla mr nci ooffice osdb reymont samba sao webster xml x-ray; do
    fpath="$CORPUS_DIR/silesia/$fname"
    if [ ! -f "$fpath" ]; then
        silesia_need_extract=1
        break
    fi
    expected="$(get_expected "$fname")"
    actual="$(compute_sha256 "$fpath")"
    if [ "$actual" != "$expected" ]; then
        printf "ERROR: silesia/%s exists but checksum mismatch!\n" "$fname" >&2
        printf "  Expected: %s\n" "$expected" >&2
        printf "  Actual:   %s\n" "$actual" >&2
        exit 1
    fi
done

if [ "$silesia_need_extract" = "1" ]; then
    printf "Extracting Silesia corpus to %s/silesia ...\n" "$CORPUS_DIR"
    mkdir -p "$CORPUS_DIR/silesia"
    if ! unzip -q -o "$SILESIA_ZIP" -d "$CORPUS_DIR/silesia"; then
        printf "ERROR: Failed to unzip %s\n" "$SILESIA_ZIP" >&2
        exit 1
    fi
    for fname in dickens mozilla mr nci ooffice osdb reymont samba sao webster xml x-ray; do
        fpath="$CORPUS_DIR/silesia/$fname"
        expected="$(get_expected "$fname")"
        actual="$(compute_sha256 "$fpath")"
        if [ "$actual" != "$expected" ]; then
            printf "ERROR: silesia/%s checksum mismatch after extraction!\n" "$fname" >&2
            printf "  Expected: %s\n" "$expected" >&2
            printf "  Actual:   %s\n" "$actual" >&2
            exit 1
        fi
        printf "silesia/%s: OK\n" "$fname"
    done
else
    printf "silesia/: OK (all 12 files already extracted and verified)\n"
fi

# 4. Check or extract enwik8.
ENWIK8_FILE="$CORPUS_DIR/enwik8"
if [ -f "$ENWIK8_FILE" ]; then
    actual="$(compute_sha256 "$ENWIK8_FILE")"
    if [ "$actual" = "$ENWIK8_RAW_SHA" ]; then
        printf "enwik8: OK (already extracted)\n"
    else
        printf "ERROR: %s exists but checksum mismatch!\n" "$ENWIK8_FILE" >&2
        printf "  Expected: %s\n" "$ENWIK8_RAW_SHA" >&2
        printf "  Actual:   %s\n" "$actual" >&2
        exit 1
    fi
else
    printf "Extracting enwik8 to %s/enwik8 ...\n" "$CORPUS_DIR"
    if ! unzip -q -o "$ENWIK8_ZIP" -d "$CORPUS_DIR"; then
        printf "ERROR: Failed to unzip %s\n" "$ENWIK8_ZIP" >&2
        exit 1
    fi
    actual="$(compute_sha256 "$ENWIK8_FILE")"
    if [ "$actual" != "$ENWIK8_RAW_SHA" ]; then
        printf "ERROR: %s checksum mismatch after extraction!\n" "$ENWIK8_FILE" >&2
        printf "  Expected: %s\n" "$ENWIK8_RAW_SHA" >&2
        printf "  Actual:   %s\n" "$actual" >&2
        exit 1
    fi
    printf "enwik8: OK\n"
fi

# 5. Write SHA256SUMS file if missing.
if [ ! -f "$CORPUS_DIR/SHA256SUMS" ]; then
    printf "Writing %s/SHA256SUMS ...\n" "$CORPUS_DIR"
    checksums_data > "$CORPUS_DIR/SHA256SUMS"
fi

# 6. Final verification pass.
verify_directory "$CORPUS_DIR"
