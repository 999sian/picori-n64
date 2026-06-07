# Project Picori — N64

A native **Nintendo 64** build of [Project Picori](https://github.com/999sian/tmc),
the PC port of the GBA decompilation of *The Legend of Zelda: The Minish Cap*
([zeldaret/tmc](https://github.com/zeldaret/tmc)). It compiles the **same
decompiled game logic** for big-endian MIPS (VR4300) via the
[libdragon](https://github.com/DragonMinded/libdragon) toolchain — it is **not an
emulator**. Research-grade / work in progress.

## How this repo is organized

The shared engine and PC bridge are **not duplicated here** — they live in the
[`tmc`](https://github.com/999sian/tmc) submodule (pinned to a specific commit).
This repo owns only the N64-exclusive delta:

| Path | What |
|---|---|
| `tmc/` | submodule — pinned Project Picori (shared `src/`, `include/`, `port/`, `libs/ViruaPPU`) |
| `patches/tmc-n64-enable.patch` | N64 enablement of shared `src/`+`port/`: big-endian `union` reorder (`__BYTE_ORDER__`), cart-safe reads (16-bit `lhu` from cart is broken on the PI bus), `TMC_N64` typedefs/byteswaps, and 32-bit-pointer layout re-gates. All gated → the PC build stays byte-identical. |
| `patches/viruappu-n64.patch` | ViruaPPU (software PPU) N64 endianness + render edits |
| `port_n64/` | N64 entry point, VBlank pad+render loop, BIOS/subsystem glue, `__wrap_memcpy` (PI-DMA cart reads) |
| `build.sh` | the build driver |

`build.sh` applies the two patches to the pinned submodule at build time
(idempotent, marker-checked), so the submodule itself stays a clean read-only
dependency. Because the patches are gated (`#ifdef TMC_N64` / `__BYTE_ORDER__` /
`__SIZEOF_POINTER__`), they are upstreamable to the `tmc` fork unchanged.

## Build

Prerequisites:
- A **libdragon** `mips64-elf` toolchain at `$N64_INST` (default `~/n64`).
- A user-supplied **`romfs/baserom.gba`** — USA, SHA1
  `b4bd50e4131b027c334547b4524e2dbbd4227130`. **Not shipped** (copyright).

```sh
# Clone WITHOUT --recurse-submodules: the tmc fork pins some private/dead
# nested submodules the N64 build doesn't need. build.sh inits exactly the
# two it does need (tmc + tmc/libs/ViruaPPU).
git clone https://github.com/999sian/picori-n64
cd picori-n64
cp /path/to/baserom.gba romfs/baserom.gba
./build.sh            # -> picori.z64   (./build.sh clean for a full rebuild)
```

Run (needs the 8 MB Expansion Pak):

```sh
ares --system "Nintendo 64" --no-file-prompt \
     --setting Nintendo64/ExpansionPak=true picori.z64
```

> **Emulator accuracy matters.** Use **ares** (the accuracy reference — what runs
> in ares matches hardware) and enable *Homebrew mode*. `gopher64` also works but
> is more lenient, so it can mask real hardware bugs (e.g. broken sub-32-bit cart
> reads). Tune to ares.

## Keeping in sync with the PC port

This repo tracks **committed** states of `tmc`. To pull newer PC-port work:

```sh
git -C tmc fetch origin && git -C tmc checkout origin/master
./build.sh clean      # re-applies the patches to the new base
git add tmc && git commit -m "bump tmc submodule"
```

If a `tmc` change collides with a patch hunk, refresh the patch: re-create it
from the working tree (`git -C tmc diff HEAD -- <files> > patches/tmc-n64-enable.patch`)
or upstream the gated edits into the `tmc` fork and shrink the patch accordingly.

## Status

**Boots and renders the title/intro correctly** (USA, ares-accurate colors). Working:
SDL-free libdragon boot, cart DMA + symbol resolution, N64 pad → input, RDP-accelerated
mode-0 BG rendering with a software (ViruaPPU) fallback, and 32bpp VI output.

### Renderer (RDP) — implemented this round
- **BG scrolling** (`BGxHOFS/VOFS`, sub-tile offset), **multi-screenblock sizes**
  (256/512), and **per-tile H/V flip** in the RDP path (`port_n64/n64_main.c`).
  *Code-complete but not yet live-verified* — see the gameplay blocker below.
- **LZ77 cart→RDRAM DMA** (`port_n64/n64_glue.c`): the GBA decompressor read the
  compressed stream one byte at a time from the uncached cart (thousands of PI-bus
  reads per tileset → multi-second room loads on emulator *and* hardware). It now
  DMAs the stream into RDRAM once and decompresses from there. Real fix, kept regardless.
- A gated **gameplay-reach harness** (`g_n64_autoplay`, demo-save → `TASK_GAME`) for
  bring-up. **Default OFF** — it currently leads into the blocker below.

### The gameplay blocker (root-caused)
Reaching `TASK_GAME` **crashes in room load** (`GameMain_InitRoom` →
`InitializeEntities`, exception @ 0). Root cause: **ROM-overlaid structs are read with
the wrong byte order on the big-endian N64.** The room property / entity data in ROM is
little-endian; reading e.g. `dat->spritePtr` natively yields `0x14DD0F08` — the
byte-swap of the valid GBA pointer `0x080FDD14` — so `ResolveRomPtr` rejects it and the
engine jumps to 0. This is *not* a localized fix: every multi-byte field of every
ROM-resident struct touched at runtime needs byte-swapping. It's the large "BIG_ENDIAN
axis" the plan calls *"a multi-month effort, not a fix-shaped change"*, and it's **the
reason the port has never gone past title/intro.** It gates gameplay, and therefore the
live verification of BG scroll, the OBJ path, affine BG, audio, save, and file-select.

See [`tmc/docs/n64-port-plan.md`](https://github.com/999sian/tmc/blob/master/docs/n64-port-plan.md)
for the full porting plan and phase status.