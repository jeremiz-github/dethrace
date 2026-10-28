#!/usr/bin/env bash
# Build dethrace as a Nintendo 64 ROM (.z64) with libdragon.
#
# Requirements: libdragon toolchain + tools installed, N64_INST pointing at it
# (default /opt/libdragon). Put the Carmageddon game data into ./n64-filesystem/
# before running: it is packed into the ROM's DFS and read from "rom:/".
set -euo pipefail

export N64_INST="${N64_INST:-/opt/libdragon}"
export PATH="$N64_INST/bin:$PATH"

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$SRC/build-n64}"
FS="${N64_FILESYSTEM:-$SRC/n64-filesystem}"
TITLE="${N64_ROM_TITLE:-DETHRACE N64}"

cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/n64-libdragon-toolchain.cmake" \
    -DDETHRACE_PLATFORM_N64=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="${N64_C_OPT:--Os -DNDEBUG}" \
    -DBRENDER_BUILD_EXAMPLES=OFF \
    -DBRENDER_BUILD_TOOLS=OFF
cmake --build "$BUILD" -j"$(nproc)"

mkdir -p "$FS" "$BUILD/rom"
if [ -z "$(ls -A "$FS")" ]; then
    echo "Put the Carmageddon game data (DATA/, *.TXT, *.SMK, ...) here and rebuild." > "$FS/README.txt"
fi

ELF="$BUILD/rom/dethrace.elf"
cp "$BUILD/dethrace" "$ELF"
n64sym "$ELF" "$ELF.sym"
cp "$ELF" "$ELF.stripped"
mips64-elf-strip -s "$ELF.stripped"
n64elfcompress -o "$BUILD/rom/" -c 1 "$ELF.stripped"
mkdfs "$BUILD/rom/dethrace.dfs" "$FS"
n64tool --title "$TITLE" --toc --output "$BUILD/dethrace.z64" \
    --align 256 "$ELF.stripped" --align 8 "$ELF.sym" --align 16 "$BUILD/rom/dethrace.dfs"

echo "ROM: $BUILD/dethrace.z64"
