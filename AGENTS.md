# Repository Guidelines

## Project Overview

**Project Picori — N64** is a native **Nintendo 64** build of [Project Picori](https://github.com/999sian/tmc) (the PC port of the GBA decompilation of *The Legend of Zelda: The Minish Cap*, [zeldaret/tmc](https://github.com/zeldaret/tmc)). It **compiles the same decompiled C game logic for big-endian MIPS (VR4300) via the [libdragon](https://github.com/DragonMinded/libdragon) SDK — it is not an emulator**.

This repo owns only the **N64-exclusive delta**: the shared engine + PC bridge live in the pinned `tmc/` submodule and are never duplicated here. N64 changes to shared code ship as build-time **patches** (gated so the PC build stays byte-identical); N64-only code lives in `port_n64/`.

A user-supplied ROM is **required and not shipped**: `romfs/baserom.gba` (USA, SHA1 `b4bd50e4131b027c334547b4524e2dbbd4227130`). It is DFS-embedded into `picori.z64` and read from the cart over the PI bus at runtime. Runs on **ares** with an **8 MB Expansion Pak**.

**Status (research-grade, WIP):** Boots and renders title/intro with ares-accurate colors; gameplay reaches rooms (room BGs + HUD render, scroll/OBJ exercised) after the cart-read/byte-order fixes; SRAM save and file-select work. Remaining: exact RDP windows/blend, audio (N64 AI + M4A), and render/ROM-DMA perf (Phase 7). The mission is **bug-fix/bring-up porting** of the existing engine to N64 — not new game logic.

## Architecture & Data Flow

Three-layer delta over the pinned `tmc/` submodule:

```
port_n64/n64_main.c::main           libdragon boot: display_init(320x240 RGBA8888), rdpq_init, joypad_init,
  │                                 check 8MB RDRAM (Expansion Pak), WireRom, then AgbMain (engine)
  ├─ WireRom (n64_main.c:69)        gRomData = KSEG1 uncached cart pointer (0xA0000000 | rom_addr); no 16MB RDRAM copy
  ├─ AgbMain (tmc/src/main.c)       unchanged engine: per-frame task dispatch → UpdateEntities → render
  │     └─ VBlankIntrWait (n64_glue.c:194) → Port_N64_VBlank (n64_main.c:331) → VBlankIntr (engine)
  └─ Port_N64_VBlank                joypad_poll → GBA KEYINPUT (active-low) → render → display_show
        ├─ Port_N64_RDP_RenderFrame (n64_main.c:175)   RDP path: mode 0/1 tiled BGs + non-affine 4bpp OBJ
        │     ├─ BG priority sort by BGxCNT[1:0] (208)  back-to-front, ties by higher BG index
        │     ├─ TMEM page batching (263)               32-tile CI4 page × palette → fewer rdpq_tex_upload
        │     ├─ affine BG2 CI8 (mode 1) (227)
        │     ├─ Port_N64_RDP_SetTexBlend (117)         BLDCNT alpha approximation over lower layers
        │     └─ Port_N64_RDP_RenderOBJ (131)           nibble-swap 4bpp CI4 + TLUT; skips affine/8bpp OBJ
        └─ software ViruaPPU fallback (n64_main.c:376)  mode 2+, hardware windows: ABGR→RGBA blit, centered 240x160
```

**Memory bridges** (the core of the port — GBA addresses are never raw-dereferenced):
- **Cart ROM** is mapped uncached **KSEG1** (`0xA0000000 | (cart & 0x1FFFFFFF)`). `__wrap_memcpy` (`n64_main.c:49`) intercepts copies whose source is in the cart domain (phys `0x10000000`–`0x1FC00000`): large aligned chunks → `dma_read` PI DMA; small/misaligned → assembled from 32-bit `io_read`. `-fno-builtin-memcpy/-memmove` is **load-bearing** so these route through the wrapper instead of inlined doubleword loads that freeze the PI bus.
- **LZ77** (`n64_glue.c::Lz77Decompress:135`) DMAs the compressed stream from cart into `sLz77DmaBuf` (96 KB) once, then decompresses from RDRAM — eliminates multi-second room loads.
- **Save**: GBA EEPROM API (`EEPROMConfigure/Read/Compare/Write0_8k_Check`, `n64_glue.c:272`) backed by **N64 cartridge SRAM** via libdragon `sram_*` (N64 EEPROM maxes at 2 KiB; TMC needs ~8 KiB). Header stamped `sram256k` by `ed64romconfig`.
- **Palettes** are byte-swapped per frame from engine LE into BE scratch tables (`sRgPlttBE`/`sObjPlttBE`) that ViruaPPU binds.

## Key Directories

| Path | Purpose |
|---|---|
| `tmc/` | **Submodule** — pinned Project Picori: shared `src/` (decompiled game logic), `include/`, `port/` (PC bridge), `libs/ViruaPPU` (software PPU). Read-only dependency; never edit it directly except to regenerate a patch. |
| `port_n64/` | **N64-exclusive glue.** `n64_main.c` (boot, cart DMA shim, RDP renderer, VBlank loop, input), `n64_glue.c` (GBA BIOS shims, LZ77 cart-DMA, SRAM save, stubs, diagnostics). Edit these directly. |
| `patches/` | Build-time patches to the submodule: `tmc-n64-enable.patch` (shared `src/`+`port/`), `viruappu-n64.patch` (software PPU). Applied idempotently (marker-checked); **regenerated from the submodule working tree, never hand-edited as raw diffs.** |
| `romfs/` | User-supplied `baserom.gba` (USA, not committed); DFS-embedded into `picori.z64`. |
| `docs/` | `phase7-rom-cache-plan.md` (perf profiling/roadmap). Full plan lives at `tmc/docs/n64-port-plan.md`. |
| `build.sh` | The entire build driver (no Makefile/CMake). |

## Development Commands

```sh
# Clone WITHOUT --recurse-submodules (the tmc fork pins dead private nested submodules;
# build.sh inits exactly tmc + tmc/libs/ViruaPPU).
git clone https://github.com/999sian/picori-n64
cd picori-n64
cp /path/to/baserom.gba romfs/baserom.gba

./build.sh            # incremental build  -> picori.z64
./build.sh clean      # full rebuild: reset submodule trees, wipe g0/ objects, re-apply patches
./build.sh -j N       # override parallel jobs (default: nproc)
```

Requires a libdragon `mips64-elf` toolchain at `$N64_INST` (default `~/n64`); GCC 15.x. `build.sh` compiles ~650 TUs (`tmc/src/*.c` except `eeprom.c` and `gba/m4a.c`; an SDL-free `tmc/port/` allowlist; `tmc/libs/ViruaPPU/src/*.c`; `port_n64/*.c`), links `engine.elf`, builds `baserom.dfs` via `mkdfs`, compresses with `n64elfcompress`, packages with `n64tool`, then stamps the SRAM save type with `ed64romconfig --savetype sram256k`.

**Run (ares is the accuracy reference — tune to it):**
```sh
ares --system "Nintendo 64" --no-file-prompt \
     --setting Nintendo64/ExpansionPak=true picori.z64    # enable Homebrew mode; needs 8MB Expansion Pak
# gopher64 also runs it but is more lenient — it masks real HW bugs (e.g. broken sub-32-bit cart reads).
```

> **Do NOT run the upstream `make`** (in the `tmc/` submodule) — that builds the GBA ROM, not the N64 port. Use `build.sh`.

### Workflow for shared-code (`tmc/`) edits
1. Edit files under `tmc/src/` or `tmc/port/` (use `fprintf(stderr, …)` under `PC_PORT`; `debugf` is libdragon-only and usable only in `port_n64/`).
2. Regenerate the patch: `git -C tmc diff HEAD -- '*.c' '*.h' > patches/tmc-n64-enable.patch` (ViruaPPU edits → `viruappu-n64.patch`).
3. `./build.sh clean` (resets the submodule and re-applies the patch from a clean base — proves the patch still lands).
4. Commit `patches/…` (and bump `tmc` only when intentionally syncing the submodule).

## Code Conventions & Common Patterns

All N64 edits to shared code are **gated** so the PC build stays byte-identical and the patch is upstreamable: `#ifdef TMC_N64`, `#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__`, or `#if … __SIZEOF_POINTER__ == 8`. Keep the `#else` branch the GBA/PC original **verbatim**. Entity/struct layout stays **GBA-size (4-byte pointers)** on N64.

Three recurring N64 bug classes:

1. **Big-endian byte order of little-endian ROM data.** GBA ROM is LE; the VR4300 is BE. Multi-byte ROM values must be byte-swapped and bit-field/union layouts reversed.
   - Union/bit-field reorder under `__BYTE_ORDER__` so aliases land on the same bits: `tmc/include/global.h` (`Coords`, `SplitDWord/Word/HWord`), `tmc/include/entity.h` (`SpriteSettings`/`SpriteRendering`/`palette`/`SpriteOrientation`).
   - Decode LE explicitly for packed ROM blobs: `ReadU32LE`/`Port_ReadU32LE` in `tmc/port/port_draw.c` (frame/OAM data); pack OAM `attr0`/`attr1` as **separate native `u16`s**, never one `memcpy`'d `u32`.
2. **ares-strict PI-bus cart-read hazard.** `lbu`/`lhu` from KSEG1 cart space race/return garbage on ares (gopher64 hides it). **Only aligned `lw` (or DMA) is cart-safe.** Fix by reading the aligned word and extracting bytes:
   ```c
   /* tmc/port/port_rom.h — Port_ReadU32 on N64 */
   if (Port_N64_IsCartPtrRange(data, 4) && ((uintptr_t)data & 3u) == 0)
       return __builtin_bswap32(*(const volatile u32*)data);   /* lw + bswap == LE byte-assembly */
   ```
   Use `Port_ReadU16`/`Port_ReadU32` for packed LE pointer tables instead of `memcpy` into a native `u32` (see `tmc/src/room.c::LoadRoomEntity`, `tmc/src/gameUtils.c::N64_ReadRoomHeader`, `tmc/port/port_linked_stubs.c::TileCollisionLookup`).
3. **Type identity (`u32`/`s32`).** mips64-newlib makes `uint32_t` a distinct type from `unsigned int`, breaking decomp prototypes. `tmc/include/gba/types.h` redefines `u32`/`s32` as `unsigned int`/`int` on N64.

Decompiled naming is preserved (`sub_080XXXXX`, `gUnk_08XXXXXX`); don't rename to guesses. N64-only stubs/BIOS impls live in `port_n64/n64_glue.c`. Per-frame diagnostics use `debugf` and should stay gated/temporary — never leave per-frame logging in a committed bring-up ROM (it shifts the PI-bus timing race). Default the gameplay-reach harness **off** (`g_n64_autoplay = 0`) before committing.

## Important Files

| File | Why it matters |
|---|---|
| `port_n64/n64_main.c` | N64 entry (`main:494`), `WireRom:69`, `Port_N64_VBlank:331`, RDP renderer (`Port_N64_RDP_RenderFrame:175`, `RenderOBJ:131`, `SetTexBlend:117`, `FrameSupported:324`), `__wrap_memcpy:49`, flags `g_n64_use_rdp:98` / `g_n64_autoplay:99` / `g_n64_rdp_obj:100`. |
| `port_n64/n64_glue.c` | GBA BIOS (`Div/Sqrt/CpuSet`), `Lz77Decompress:135` (cart→RDRAM DMA), SRAM save (`EEPROM*:272`), `VBlankIntrWait:194`, non-fatal `abort`/assert + `g_n64_last_bad_addr` diagnostics, link-only stubs. |
| `build.sh` | The whole build: submodule init, patch apply, compile/link flags, DFS + `n64tool` packaging, `ed64romconfig` save stamp. |
| `patches/tmc-n64-enable.patch` | All gated N64 edits to shared `src/`+`port/` (BE unions, cart-safe reads, type fixes). |
| `patches/viruappu-n64.patch` | ViruaPPU N64 edits (LE tilemap reads, native BE IO regs, `__thread`→file-scope, mosaic, widescreen). |
| `tmc/port/port_rom.h` / `port_rom.c` | Cart↔native ROM bridge: `Port_N64_IsCartPtrRange`, cart-safe `Port_ReadU16/U32`, packed-pointer decode. |
| `README.md`, `docs/phase7-rom-cache-plan.md` | Project status, run instructions, perf roadmap. |

## Runtime/Tooling Preferences

- **Toolchain:** libdragon `mips64-elf` GCC at `$N64_INST` (default `~/n64`). No Make/CMake/Node — `build.sh` (bash) is the only driver.
- **Compile flags:** `-march=vr4300 -mtune=vr4300 -mabi=o64 -G0 -O2 -std=gnu17 -ffunction-sections -fdata-sections -fno-strict-aliasing -fwrapv -fno-builtin-memcpy -fno-builtin-memmove`. `-fno-builtin-memcpy/-memmove` and `-G0` are load-bearing.
- **Defines:** `-DN64 -DPC_PORT -DTMC_N64 -DNON_MATCHING -DUSE_HDMA -DUSA -DENGLISH -DREVISION=0`. Include dirs must include `$ROOT/build/USA` (generated headers) plus `tmc/include`, `tmc/port`, and ViruaPPU `include`.
- **Hardware:** 8 MB Expansion Pak required (image > 4 MB base RDRAM). ares video driver must be OpenGL 3.2+.
- **Submodules:** init only `tmc` and `tmc/libs/ViruaPPU`; **never** `--recurse-submodules` (dead nested refs in the fork).
- **Reference emulator:** **ares** (hardware-accurate; a bug in ares-not-gopher64 is a real HW bug). gopher64 is lenient and its `Exhausted LinkedDeviceHost memory` OOM is a per-upload Vulkan quirk, not a HW issue.

## Testing & QA

**No automated test suite and no CI test gate.** Verification is interactive/tool-assisted on ares:

- **Headless bring-up harness:** `g_n64_autoplay=1` (in `port_n64/n64_main.c`) drives title→file-select/gameplay and can `Port_N64_ForceGameStart()` into the demo-save room; restore to `0` before committing. ares gameplay runs take ~3–5 min to reach a room.
- **Framebuffer verification:** N64 dumps an ASCII thumbnail / RGB hex via `debugf` (frame-gated, e.g. f=300) reconstructed with Python/PIL; the PC port is ground truth — capture it with `TMC_PUBLISH_FRAMEBUFFER=1 TMC_AUTOPLAY=1 SDL_VIDEODRIVER=dummy ./tmc_pc` and diff. ares/gopher64 windows don't reliably screenshot under X11/Wayland — prefer the hex dump.
- **Crash symbolication:** `addr2line -e engine.elf.stripped -fpi <addr>`; walk the `fp[]` chain from the crash dump (the handler backtrace doesn't cross the signal frame).
- **Compile-time gate:** `PORT_STATIC_ASSERT_OFFSET/SIZE` (shared `tmc/include`) lock struct-layout fixes.
- **Method:** profile/diagnose on **ares**, not gopher64; bisect blockers by instrumenting one pipeline stage at a time with gated `debugf` counters, build, boot, grep the log. State which area/repro you verified in commit messages.

Commits: `n64: <short description>` (e.g. `n64: fix ares cart-safe room bg rendering`); one logical fix per commit.
