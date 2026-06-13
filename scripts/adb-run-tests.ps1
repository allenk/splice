# ─── PowerShell helper: push splice tests to device and run ───────────────
# Usage:
#   scripts/adb-run-tests.ps1                            # default android-arm64-dev
#   scripts/adb-run-tests.ps1 android-arm64-release
#
# Prerequisites:
#   - adb on PATH; `adb devices` shows your phone
#   - device has adb root (best) or /data/local/tmp writable + execable
#   - Splice has been built for that preset
# ───────────────────────────────────────────────────────────────────────────

param(
    [string]$Preset = "android-arm64-dev"
)

$ErrorActionPreference = "Stop"

$BinDir = "out/build/$Preset/tests"
$Remote = "/data/local/tmp/splice"

if (-not (Test-Path $BinDir)) {
    Write-Error "Build dir not found: $BinDir`nRun: cmake --build --preset=$Preset  first"
    exit 1
}

Write-Host "─── Preparing remote dir $Remote ───"
adb shell "mkdir -p $Remote"
adb shell "chmod 0755 $Remote"

Write-Host "─── Pushing test binaries ───"
adb push "$BinDir/splice_smoke_test" "$Remote/"
adb push "$BinDir/splice_unit_test"  "$Remote/"
adb shell "chmod 0755 $Remote/splice_smoke_test $Remote/splice_unit_test"

Write-Host ""
Write-Host "─── Running splice_smoke_test on device ───"
adb shell "$Remote/splice_smoke_test --gtest_color=yes"

Write-Host ""
Write-Host "─── Running splice_unit_test on device ───"
adb shell "$Remote/splice_unit_test --gtest_color=yes"

Write-Host ""
Write-Host "─── Done ───"
Write-Host "Tip: Splice logs go through __android_log_print on Android."
Write-Host "     Watch them with:  adb logcat -s splice"
