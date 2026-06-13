#!/usr/bin/env bash
# ─── Push splice tests to a connected Android device and run them ──────────
#
# Usage:
#   scripts/adb-run-tests.sh                  # default preset android-arm64-dev
#   scripts/adb-run-tests.sh android-arm64-release
#
# Prerequisites:
#   - adb on PATH and `adb devices` shows your device
#   - device has either adb root (best) or /data/local/tmp writable + execable
#   - Splice has been built for that preset (cmake --build --preset=<name>)
# ───────────────────────────────────────────────────────────────────────────

set -euo pipefail

PRESET="${1:-android-arm64-dev}"
BIN_DIR="out/build/${PRESET}/tests"
REMOTE="/data/local/tmp/splice"

if [ ! -d "$BIN_DIR" ]; then
    echo "Build dir not found: $BIN_DIR"
    echo "Run:  cmake --build --preset=${PRESET}  first"
    exit 1
fi

echo "─── Preparing remote dir ${REMOTE} ───"
adb shell "mkdir -p ${REMOTE}"
adb shell "chmod 0755 ${REMOTE}"

echo "─── Pushing test binaries ───"
adb push "${BIN_DIR}/splice_smoke_test" "${REMOTE}/"
adb push "${BIN_DIR}/splice_unit_test"  "${REMOTE}/"
adb shell "chmod 0755 ${REMOTE}/splice_smoke_test ${REMOTE}/splice_unit_test"

echo
echo "─── Running splice_smoke_test on device ───"
adb shell "${REMOTE}/splice_smoke_test --gtest_color=yes" || true

echo
echo "─── Running splice_unit_test on device ───"
adb shell "${REMOTE}/splice_unit_test --gtest_color=yes"

echo
echo "─── Done ───"
echo "Tip: Splice logs go through __android_log_print on Android."
echo "     Watch them with:  adb logcat -s splice"
