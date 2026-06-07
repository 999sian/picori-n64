#!/usr/bin/env bash
#
# build.sh — Project Picori (N64) build driver.
#
# This repo is a THIN N64 build wrapper over the Minish Cap PC port
# (Project Picori). The shared decompiled engine + PC bridge live in the
# `tmc/` submodule (a pinned commit of github.com/999sian/tmc); this repo owns
# only the N64-exclusive parts:
#   - patches/tmc-n64-enable.patch   N64 enablement of shared src/ + port/
#                                    (big-endian unions, cart-safe reads, TMC_N64
#                                    typedefs/byteswaps, 32-bit-ptr layout re-gates)
#   - patches/viruappu-n64.patch     ViruaPPU N64 endianness + render edits
#   - port_n64/                      N64 entry, VBlank pad+render, glue/stubs
#   - build.sh / this driver
#
# The patches apply to the pinned submodule at build time (idempotent, marker-
# checked) so the submodule itself stays a clean, read-only dependency. They are
# gated (#ifdef TMC_N64 / __BYTE_ORDER__ / __SIZEOF_POINTER__) and keep the PC
# build byte-identical, so they are upstreamable to the tmc fork at any time.
#
# Usage:
#   ./build.sh            incremental (recompile changed TUs, relink, repackage)
#   ./build.sh clean      reset submodule, wipe objects, full rebuild
#   ./build.sh -j N       override parallel compile jobs (default: nproc)
#
# Requires: a libdragon mips64-elf toolchain at $N64_INST (default ~/n64) and a
# user-supplied romfs/baserom.gba (USA; copyright, not shipped).
set -euo pipefail

export N64_INST="${N64_INST:-$HOME/n64}"
OUTER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$OUTER/tmc"                       # the pinned tmc submodule (shared source)
VPPU="$ROOT/libs/ViruaPPU"
OBJ="$OUTER/g0"
CC="$N64_INST/bin/mips64-elf-gcc"
CXX="$N64_INST/bin/mips64-elf-g++"
LIBDIR="$N64_INST/mips64-elf/lib"

JOBS="$(nproc 2>/dev/null || echo 4)"
DO_CLEAN=0
while [ $# -gt 0 ]; do
  case "$1" in
    clean) DO_CLEAN=1; shift ;;
    -j) JOBS="$2"; shift 2 ;;
    -j*) JOBS="${1#-j}"; shift ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
done

[ -x "$CC" ] || { echo "ERROR: $CC missing (set N64_INST to your libdragon toolchain)"; exit 1; }

# ---- submodule + patches ----------------------------------------------------
if [ ! -e "$ROOT/.git" ]; then
  echo "[submodule] initializing tmc + nested ViruaPPU"
  git -C "$OUTER" submodule update --init --recursive
fi

if [ "$DO_CLEAN" = 1 ]; then
  echo "[clean] reset submodule trees + rm objects"
  git -C "$ROOT" checkout -q -- . 2>/dev/null || true
  git -C "$VPPU" checkout -q -- . 2>/dev/null || true
  rm -rf "$OBJ"
fi

# Apply the N64 patches to the pinned submodule. Marker-checked so repeat builds
# don't churn: if fade.c already has the cart-read fix, the tree is patched.
if ! grep -q "n64_cart_u16" "$ROOT/src/fade.c" 2>/dev/null; then
  echo "[patch] applying N64 patches to submodule (pinned $(git -C "$ROOT" rev-parse --short HEAD))"
  git -C "$ROOT" checkout -q -- . 2>/dev/null || true
  git -C "$VPPU" checkout -q -- . 2>/dev/null || true
  git -C "$ROOT" apply --whitespace=nowarn "$OUTER/patches/tmc-n64-enable.patch"
  git -C "$VPPU" apply --whitespace=nowarn "$OUTER/patches/viruappu-n64.patch"
fi

# -I$ROOT/build/USA resolves the committed generated `assets/{gfx,map}_offsets.h`.
INCLUDE="-I$N64_INST/mips64-elf/include -I$ROOT/include -I$ROOT/port -I$ROOT -I$ROOT/build/USA -I$VPPU/include"
DEFINES="-DN64 -DPC_PORT -DNON_MATCHING -DUSE_HDMA -DUSA -DENGLISH -DREVISION=0 -DTMC_N64"
# vr4300/o64/-G0 match the known-good objects. -fno-builtin-memcpy/-memmove are
# load-bearing: gRomData is an uncached cart pointer and at -O2 GCC inlines
# fixed-size memcpy into ldl/ldr doubleword loads; a 64-bit CPU read from the PI
# bus freezes the console. Keeping memcpy a real call routes cart sources through
# __wrap_memcpy's PI DMA (port_n64/n64_main.c).
CFLAGS="-march=vr4300 -mtune=vr4300 -mabi=o64 -G0 \
  -falign-functions=32 -ffunction-sections -fdata-sections -g \
  -O2 -std=gnu17 -fno-strict-aliasing -fwrapv -fno-strict-overflow \
  -fno-builtin-memcpy -fno-builtin-memmove -w \
  $DEFINES $INCLUDE"

# ---- source set -------------------------------------------------------------
# Shared engine: all tmc/src/*.c except the three the port replaces — eeprom.c
# (save hardware), fileselect.c (host SDL/glibc), gba/m4a.c (audio engine).
mapfile -t SRCS < <(cd "$ROOT" && find src -name '*.c' \
  ! -name eeprom.c ! -name fileselect.c ! -path 'src/gba/m4a.c' | sort | sed "s#^#$ROOT/#")

# Reused PC bridge (allowlist: desktop/SDL TUs excluded) + ViruaPPU, from the
# submodule. The PC bug-repro harnesses (port_repro_*.c) are PC-only scaffolding
# driven by port_bios.c (excluded on N64) — omitted here.
PORT_REL=(
  port/data_const_stubs.c port/data_stubs_autogen.c
  port/port_analog_movement.c port/port_animation.c port/port_asset_index.c
  port/port_bugreport_state.c port/port_debug_actions.c port/port_debug_verbose.c
  port/port_draw.c port/port_figurines.c port/port_filter.c
  port/port_gameplay_stubs.c port/port_gba_mem.c port/port_hdma.c
  port/port_inline_ptrs.c port/port_linked_stubs.c port/port_m4a_stubs.c
  port/port_math.c port/port_rom.c
  port/port_rom_tables.c port/port_room_funcs.c port/port_script_funcs.c
  port/port_shm_framebuffer.c port/port_stubs.c port/port_text_render.c
  port/port_upscale.c port/stubs_autogen.c
  libs/ViruaPPU/src/mode0.c libs/ViruaPPU/src/mode1.c libs/ViruaPPU/src/mode2.c
  libs/ViruaPPU/src/mode7.c libs/ViruaPPU/src/virtuappu.c
)
PORT_SRCS=(); for f in "${PORT_REL[@]}"; do PORT_SRCS+=("$ROOT/$f"); done
# N64-exclusive glue lives in THIS repo, not the submodule.
GLUE=( "$OUTER/port_n64/n64_glue.c" "$OUTER/port_n64/n64_main.c" )

ALL_SRCS=( "${SRCS[@]}" "${PORT_SRCS[@]}" "${GLUE[@]}" )

mkdir -p "$OBJ"

# Map an absolute source path to a unique object filename.
obj_of() { local p="${1#/}"; echo "$OBJ/${p//\//__}.o"; }

# ---- compile (parallel, incremental) ---------------------------------------
compile_one() {
  local src="$1" obj; obj="$(obj_of "$1")"
  if [ -f "$obj" ] && [ "$obj" -nt "$src" ]; then return 0; fi
  echo "  [CC] ${src#"$OUTER"/}"
  # shellcheck disable=SC2086
  "$CC" -c $CFLAGS -o "$obj" "$src"
}
export -f compile_one obj_of
export CC CFLAGS OBJ OUTER

echo "[build] ${#ALL_SRCS[@]} TUs, -j$JOBS"
printf '%s\n' "${ALL_SRCS[@]}" | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}

# ---- link -------------------------------------------------------------------
echo "[link] engine.elf"
mapfile -t OBJS < <(for s in "${ALL_SRCS[@]}"; do obj_of "$s"; done)
"$CXX" -o "$OUTER/engine.elf" "${OBJS[@]}" -lc -mabi=o64 \
  -Wl,-g -Wl,-L"$LIBDIR" \
  -Wl,-ldragon -Wl,-lm -Wl,-ldragonsys \
  -Wl,-Tn64.ld -Wl,--gc-sections \
  -Wl,--wrap,__do_global_ctors \
  -Wl,--wrap,memcpy -Wl,--wrap,__inspector_assertion \
  -Wl,-Map="$OUTER/engine.map"
"$N64_INST/bin/mips64-elf-size" "$OUTER/engine.elf"

# ---- package ----------------------------------------------------------------
[ -f "$OUTER/romfs/baserom.gba" ] || {
  echo "ERROR: romfs/baserom.gba missing — supply your own USA ROM"
  echo "       (SHA1 b4bd50e4131b027c334547b4524e2dbbd4227130), it is not shipped."
  exit 1
}
echo "[dfs] baserom.dfs"
if [ ! -f "$OUTER/baserom.dfs" ] || [ "$OUTER/romfs/baserom.gba" -nt "$OUTER/baserom.dfs" ]; then
  "$N64_INST/bin/mkdfs" "$OUTER/baserom.dfs" "$OUTER/romfs" >/dev/null
fi

echo "[z64] picori.z64"
"$N64_INST/bin/n64sym" "$OUTER/engine.elf" "$OUTER/engine.elf.sym"
cp "$OUTER/engine.elf" "$OUTER/engine.elf.stripped"
"$N64_INST/bin/mips64-elf-strip" -s "$OUTER/engine.elf.stripped"
# Wrap the stripped ELF in libdragon's compressed container (IPL3 decompresses
# at boot). Matches the known-good ROM; halves the embedded ELF.
"$N64_INST/bin/n64elfcompress" -o "$OUTER" -c 1 "$OUTER/engine.elf.stripped" >/dev/null
"$N64_INST/bin/n64tool" --title "Picori" --toc --output "$OUTER/picori.z64" \
  --align 256 "$OUTER/engine.elf.stripped" \
  --align 8 "$OUTER/engine.elf.sym" \
  --align 16 "$OUTER/baserom.dfs"

ls -la "$OUTER/picori.z64"
echo "[done] picori.z64 ready"
echo "  run: ares --system \"Nintendo 64\" --no-file-prompt --setting Nintendo64/ExpansionPak=true \"$OUTER/picori.z64\""
