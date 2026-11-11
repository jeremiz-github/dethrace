#!/bin/bash
# Build an emulator test ROM (demo DATA inside the ROM) and run it headless in the patched cen64.
# usage: tools/emu-run.sh <seconds> <frames-dir> [frames-every]
# Output: game log (IS-Viewer) in $FRAMES_DIR/log.txt, PPM frames in $FRAMES_DIR.
set -e
export N64_INST=/opt/libdragon PATH=/opt/libdragon/bin:$PATH
export BUILD=${BUILD:-/home/claude/bt-emu} N64_FILESYSTEM=${N64_FILESYSTEM:-/home/claude/fsroot}
export N64_C_OPT="${N64_C_OPT:--Os -DNDEBUG -fno-math-errno -DN64_EMU}"
export LDFLAGS="-Wl,--wrap=sys_hw_memset -Wl,--wrap=sys_hw_memset16 -Wl,--wrap=sys_hw_memset32 -Wl,--wrap=sys_hw_memset64"
cd "$(dirname "$0")/.."
[ -n "$SKIP_BUILD" ] || tools/build-n64.sh > /home/claude/build.log 2>&1 || { tail -20 /home/claude/build.log; exit 1; }
mkdir -p "$2"
CEN64_ELF=$BUILD/rom/dethrace.elf CEN64_FRAMES_DIR=$2 CEN64_FRAMES_EVERY=${3:-60} \
  timeout "$1" stdbuf -o0 /home/claude/cen64/build/cen64 -headless -is-viewer /home/claude/emu/pifrom.bin $BUILD/dethrace.z64 > "$2/log.txt" 2>&1 || true
grep -c "^\[vi" "$2/log.txt" | sed 's/^/vi frames: /'
