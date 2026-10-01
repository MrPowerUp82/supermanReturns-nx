#!/usr/bin/env bash
# Runs an NRO in a Linux build of Sudachi's SDL frontend (sudachi-cmd) without a
# desktop, with an isolated portable profile, and keeps the logs.
#
#   SUDACHI_CMD=/path/to/sudachi-cmd tools/switch/run-sudachi-headless.sh NRO [SECONDS]
#
# Environment:
#   BACKEND        0 OpenGL, 1 Vulkan, 2 null renderer (default 2). The null
#                  renderer still runs the emulated GPU command processor
#                  (GPFIFO, Maxwell 3D methods, MME macros), only host drawing
#                  is skipped. Vulkan needs a host ICD (lavapipe works).
#   ASYNC_GPU      0/1 (default 1, as Sudachi's default)
#   MACRO_JIT      0 to set disable_macro_jit=true (default 1: JIT enabled)
#   GPU_ACCURACY   0 normal, 1 high (Sudachi's default), 2 extreme
#   DUMP_MACROS    1 to dump every MME macro the guest uploads to dump/macros/
#   PROBE_CFG      optional file copied to sdmc:/switch/superman-returns-nx/vk-probe.cfg
#   OUT            output directory (default out/sudachi-linux/<timestamp>)
#   GDB            1 to run under gdb in batch mode and print a backtrace on a crash
#
# Exit status: the emulator's, 124 when the timeout ended it (137 if it had to
# be killed). Remaining open until the timeout is not evidence that the guest
# works; read vk-probe.log.
set -euo pipefail

NRO=$(realpath "${1:?usage: $0 NRO [SECONDS]}")
SECONDS_LIMIT=${2:-60}
SUDACHI_CMD=$(realpath "${SUDACHI_CMD:?set SUDACHI_CMD to the sudachi-cmd binary}")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/out/sudachi-linux/$(date +%Y%m%d-%H%M%S)}
BACKEND=${BACKEND:-2}
ASYNC_GPU=${ASYNC_GPU:-1}
MACRO_JIT=${MACRO_JIT:-1}
GPU_ACCURACY=${GPU_ACCURACY:-1}

mkdir -p "$OUT/run/user/config" "$OUT/run/user/sdmc/switch/superman-returns-nx"
OUT=$(realpath "$OUT")
# Sudachi's INI keeps a "key\default" flag next to each key; a value is only
# applied when its flag is false.
setting() { printf '%s=%s\n%s\\default=false\n' "$1" "$2" "$1"; }
{
    echo "[Renderer]"
    setting backend "$BACKEND"
    setting use_asynchronous_gpu_emulation "$([ "$ASYNC_GPU" = 0 ] && echo false || echo true)"
    setting use_disk_shader_cache false
    setting gpu_accuracy "$GPU_ACCURACY"
    echo "[Debugging]"
    setting disable_macro_jit "$([ "$MACRO_JIT" = 0 ] && echo true || echo false)"
    setting dump_macros "$([ "${DUMP_MACROS:-0}" = 1 ] && echo true || echo false)"
    echo "[Miscellaneous]"
    setting log_filter "*:Info Service.NVDRV:Debug HW.GPU:Debug Debug.Emulated:Debug"
} > "$OUT/run/user/config/sdl2-config.ini"
if [ -n "${PROBE_CFG:-}" ]; then
    cp "$PROBE_CFG" "$OUT/run/user/sdmc/switch/superman-returns-nx/vk-probe.cfg"
fi

# sudachi-cmd needs a display for its SDL window, also with the null renderer.
XVFB_PID=
if [ -z "${DISPLAY:-}" ]; then
    export DISPLAY=:97
    Xvfb "$DISPLAY" -screen 0 1280x720x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1
fi
cleanup() { [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null || true; }
trap cleanup EXIT

status=0
cd "$OUT/run"
if [ "${GDB:-0}" = 1 ]; then
    # Dynarmic and fastmem raise SIGSEGV on purpose; only stop on the fatal one.
    timeout --kill-after=15 --signal=INT "$SECONDS_LIMIT" gdb -q -batch \
        -ex "handle SIGSEGV nostop noprint pass" -ex "handle SIGBUS nostop noprint pass" \
        -ex "handle SIGUSR1 nostop noprint pass" -ex "handle SIGUSR2 nostop noprint pass" \
        -ex run -ex "thread apply all bt 12" \
        --args "$SUDACHI_CMD" "$NRO" > "$OUT/gdb.txt" 2>&1 || status=$?
else
    timeout --kill-after=15 "$SECONDS_LIMIT" "$SUDACHI_CMD" "$NRO" > "$OUT/stdout.txt" 2>&1 || status=$?
fi
cp "$OUT/run/user/log/sudachi_log.txt" "$OUT/" 2>/dev/null || true
[ -d "$OUT/run/user/dump/macros" ] && cp -r "$OUT/run/user/dump/macros" "$OUT/"
cp "$OUT/run/user/sdmc/switch/superman-returns-nx/"*.log "$OUT/" 2>/dev/null || true
echo "emulator exit status: $status (124 = timeout)"
echo "logs: $OUT"
if [ -f "$OUT/vk-probe.log" ]; then
    grep -E "^(BEGIN|END|RESULT|stop_after)" "$OUT/vk-probe.log" | tail -n 25
else
    echo "no vk-probe.log was written"
fi
exit "$status"
