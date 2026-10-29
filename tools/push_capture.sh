#!/bin/sh
# Push vkpc_replay and a capture file to the device.
# Usage: tools/push_capture.sh <capture.vkpc>      Env: ADB (default adb)
set -eu
ADB=${ADB:-adb}
DIR=/data/local/tmp/powervr-repros
cd "$(dirname "$0")/.."
$ADB shell mkdir -p $DIR
$ADB push build/vkpc_replay $DIR/ >/dev/null
$ADB shell chmod 755 $DIR/vkpc_replay
$ADB push "$1" $DIR/ >/dev/null
echo "on device: $DIR/vkpc_replay $DIR/$(basename "$1")"
