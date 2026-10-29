#!/bin/sh
# Push the repros to /data/local/tmp and run every case; each case runs in its own process (with a
# 120 s timeout) so a driver crash or hang in one does not hide the others. Output: results.txt,
# plus the crash-buffer lines from logcat for crashing cases.
# Usage: ./run_on_device.sh [adb serial]
set -u
cd "$(dirname "$0")"
ADB="adb${1:+ -s $1}"
DIR=/data/local/tmp/powervr-repros
$ADB shell mkdir -p $DIR
for f in build/repro_*; do $ADB push "$f" $DIR/ >/dev/null; done
$ADB shell chmod 755 "$DIR/repro_*"

run() {
    echo "=== $*"
    $ADB logcat -c
    $ADB shell "cd $DIR && timeout 120 ./$*; echo exit=\$?"
    $ADB logcat -d -b crash | grep -E "signal|Abort|backtrace|#0[0-9] " | head -n 20
    echo
}

{
    $ADB shell getprop ro.product.model
    $ADB shell getprop ro.build.fingerprint
    echo
    run repro_view_format control
    run repro_view_format srgb-image-unorm-view
    run repro_view_format unorm-image-srgb-view
    run repro_view_format srgb-image-unorm-view nodraw
    run repro_view_format unorm-image-srgb-view nodraw
    run repro_unused_attachment control
    run repro_unused_attachment unused-nowrite
    run repro_unused_attachment unused-write
    run repro_unused_attachment unused-nowrite msaa2
    run repro_unused_attachment unused-write msaa2
    run repro_compute_cache inprocess
    run repro_compute_cache mixed
    $ADB shell rm -f $DIR/cache.bin
    run repro_compute_cache save cache.bin
    run repro_compute_cache load cache.bin
    run repro_concurrent_compile 4 60 serial
    run repro_concurrent_compile 8 150
} 2>&1 | tee results.txt
