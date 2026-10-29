#!/bin/sh
# Push the repros to /data/local/tmp and run every case; each case runs in its own process so a
# driver crash in one does not hide the others. Output: results.txt (and the tombstone pointer
# from logcat for crashing cases).
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
    $ADB shell "cd $DIR && ./$*; echo exit=\$?"
    $ADB logcat -d -b crash | grep -E "signal|Abort|backtrace|#0[0-9] " | head -n 20
    echo
}

{
    $ADB shell getprop ro.product.model
    $ADB shell getprop ro.build.fingerprint
    echo
    run repro_unused_attachment control
    run repro_unused_attachment unused-nowrite
    run repro_unused_attachment unused-write
    run repro_compute_cache inprocess
    $ADB shell rm -f $DIR/cache.bin
    run repro_compute_cache save cache.bin
    run repro_compute_cache load cache.bin
    run repro_concurrent_compile 4 60 serial
    run repro_concurrent_compile 4 60
    run repro_view_format control
    run repro_view_format srgb-image-unorm-view
    run repro_view_format srgb-image-unorm-view nodraw
    run repro_view_format unorm-image-srgb-view
    run repro_view_format unorm-image-srgb-view nodraw
} 2>&1 | tee results.txt
