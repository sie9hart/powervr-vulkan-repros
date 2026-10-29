#!/bin/sh
# Reduce one shader stage of a captured pipeline that makes the driver fault, with spirv-reduce.
# Every candidate is pushed to the device and replayed alone; a candidate is kept only when the
# replay still dies from a signal. The result is a minimal SPIR-V module that faults the driver,
# independent of the application that produced the capture.
#
# Usage: tools/reduce_shader.sh <capture.vkpc> <pipeline-index> <stage> [workdir]
#   stage: vert | frag | comp | geom | tesc | tese
# Env:   ADB (default adb; on WSL use adb.exe), SPIRV_REDUCE (default: spirv-reduce from the NDK
#        when ANDROID_NDK is set, else from PATH), REPLAY_ARGS (extra vkpc_replay options,
#        e.g. "--no-seed").
# Needs build/vkpc_replay and the capture already on the device: run tools/push_capture.sh first.
set -eu
[ $# -ge 3 ] || { echo "usage: $0 <capture.vkpc> <pipeline-index> <stage> [workdir]" >&2; exit 1; }
capture=$1; index=$2; stage=$3; work=${4:-reduce-$index-$stage}
ADB=${ADB:-adb}
DIR=/data/local/tmp/powervr-repros
if [ -z "${SPIRV_REDUCE:-}" ]; then
    if [ -n "${ANDROID_NDK:-}" ]; then SPIRV_REDUCE=$(ls "$ANDROID_NDK"/shader-tools/*/spirv-reduce | head -n 1)
    else SPIRV_REDUCE=spirv-reduce; fi
fi
mkdir -p "$work"
work=$(cd "$work" && pwd)
remote_capture="$DIR/$(basename "$capture")"

# 1. The original stage, extracted on the device (the capture lives there).
$ADB shell "cd $DIR && ./vkpc_replay $(basename "$capture") --dump-spv $index $DIR" >/dev/null
$ADB pull "$DIR/$stage.spv" "$work/original.spv" >/dev/null

# 2. Interestingness test: faults on the device with this candidate swapped in.
cat > "$work/interesting.sh" <<EOF
#!/bin/sh
cand=\$1
$ADB push "\$cand" $DIR/candidate.spv >/dev/null 2>&1 || exit 1
code=\$($ADB shell "cd $DIR && timeout 60 ./vkpc_replay $remote_capture --only $index --spv $stage=candidate.spv ${REPLAY_ARGS:-} >/dev/null 2>&1; echo \\\$?" | tr -d '\r')
# 128+signal: SIGSEGV 139, SIGABRT 134, SIGBUS 135. A timeout (124) or VkResult failure is not it.
case "\$code" in 134|135|139) exit 0 ;; *) exit 1 ;; esac
EOF
chmod +x "$work/interesting.sh"

echo "checking that the original stage faults..."
if ! "$work/interesting.sh" "$work/original.spv"; then
    echo "the original pipeline does not fault on its own with these options; nothing to reduce" >&2
    exit 2
fi
echo "reducing $(wc -c < "$work/original.spv") bytes of SPIR-V (this takes a while)..."
"$SPIRV_REDUCE" "$work/original.spv" -o "$work/reduced.spv" -- "$work/interesting.sh"
echo "reduced: $(wc -c < "$work/reduced.spv") bytes -> $work/reduced.spv"
echo "disassemble with: spirv-dis $work/reduced.spv"
