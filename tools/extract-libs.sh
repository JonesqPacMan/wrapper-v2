#!/usr/bin/env bash
# extract-libs.sh - Extract Apple native libraries from an APKMirror .apkm bundle
# or a standalone arch split .apk, and verify each .so against LIBS_VERSION.json.
#
# The bundle/APK file itself is not hashed; only extracted libraries are checked.
#
# Usage:
#   extract-libs.sh --bundle <path-to-.apkm|.apk> [--arch <x86_64|arm64-v8a>] [--out <dir>]
#
# Options:
#   --arch <x86_64|arm64-v8a>    Which arch's libs to extract (default x86_64)
#   --out  <directory>           Where to drop the .so files (default: <repo>/rootfs/system/lib64)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBS_VERSION="$REPO_ROOT/LIBS_VERSION.json"

BUNDLE=""
ARCH="x86_64"
OUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bundle) BUNDLE="$2"; shift 2 ;;
        --arch)   ARCH="$2";   shift 2 ;;
        --out)    OUT="$2";    shift 2 ;;
        -h|--help)
            sed -n '2,14p' "$0"
            exit 0
            ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$BUNDLE" ]]; then
    echo "extract-libs: missing --bundle <path-to-.apkm|.apk>" >&2
    exit 2
fi
if [[ ! -f "$BUNDLE" ]]; then
    echo "extract-libs: not a file: $BUNDLE" >&2
    exit 2
fi
if [[ -z "$OUT" ]]; then
    OUT="$REPO_ROOT/rootfs/system/lib64"
fi

case "$ARCH" in
    x86_64)    SPLIT_NAME="split_config.x86_64.apk"    ;;
    arm64-v8a) SPLIT_NAME="split_config.arm64_v8a.apk" ;;
    *) echo "extract-libs: unsupported arch '$ARCH'" >&2; exit 2 ;;
esac
APK_LIB_DIR="lib/$ARCH"

for c in jq sha256sum unzip; do
    command -v "$c" >/dev/null || { echo "extract-libs: $c is required" >&2; exit 3; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Resolve the arch split APK: .apkm wraps split_config.*.apk; a bare .apk is used as-is.
APK=""
bundle_lower="${BUNDLE,,}"
if [[ "$bundle_lower" == *.apkm ]]; then
    unzip -qq "$BUNDLE" "$SPLIT_NAME" -d "$TMP"
    APK="$TMP/$SPLIT_NAME"
    [[ -f "$APK" ]] || {
        echo "extract-libs: bundle missing $SPLIT_NAME (wrong .apkm or --arch?)" >&2
        exit 6
    }
elif [[ "$bundle_lower" == *.apk ]]; then
    APK="$BUNDLE"
else
    echo "extract-libs: expected .apkm or .apk extension: $BUNDLE" >&2
    exit 2
fi

# Sanity-check the APK contains libs for the requested arch.
if ! unzip -l "$APK" "$APK_LIB_DIR/" 2>/dev/null | grep -q "$APK_LIB_DIR/"; then
    echo "extract-libs: $APK has no $APK_LIB_DIR/ (wrong split or --arch?)" >&2
    exit 6
fi

mkdir -p "$OUT"
LIB_TMP="$TMP/libs"
mkdir -p "$LIB_TMP"
unzip -qq "$APK" "$APK_LIB_DIR/*" -d "$LIB_TMP"

# `jq.exe` on msys2/MSVC emits CRLF on Windows; strip CR defensively before
# iterating, otherwise lib names get a stray \r appended and every lookup fails.
mapfile -t EXPECTED_LIBS < <(
    jq -r --arg arch "$ARCH" '.libs[$arch] | keys[]' "$LIBS_VERSION" | tr -d '\r'
)
[[ ${#EXPECTED_LIBS[@]} -gt 0 ]] || {
    echo "extract-libs: no libs pin for arch '$ARCH' in LIBS_VERSION.json" >&2
    exit 4
}

ok=0
fail=0
for so in "${EXPECTED_LIBS[@]}"; do
    src="$LIB_TMP/$APK_LIB_DIR/$so"
    if [[ ! -f "$src" ]]; then
        echo "extract-libs: missing in APK: $so" >&2
        fail=$((fail+1))
        continue
    fi
    expect="$(jq -r --arg arch "$ARCH" --arg so "$so" '.libs[$arch][$so]' "$LIBS_VERSION" | tr -d '\r')"
    actual="$(sha256sum "$src" | awk '{print $1}')"
    if [[ "$expect" != "$actual" ]]; then
        echo "extract-libs: SHA-256 mismatch on $so" >&2
        echo "  expected: $expect" >&2
        echo "  actual:   $actual" >&2
        fail=$((fail+1))
        continue
    fi
    install -m 0644 "$src" "$OUT/$so"
    ok=$((ok+1))
done

echo "extract-libs: $ok ok, $fail failed (arch=$ARCH out=$OUT)"
[[ $fail -eq 0 ]]
