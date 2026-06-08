> **CORRECTION (2026-06-08, after profiling — this re-prioritizes the doc).**
> The "profile first" step disproved the premise below. With heartbeats in
> `GameMain_InitRoom`/`InitializeEntities`, gopher64 prints **`[gir] InitRoom DONE`**
> — i.e. the gameplay room **loads fine and fast** (all 14 init sub-calls + DMA/LZ77
> bulk loads complete). The stall is **not** ROM-read perf. The log then ends with
> gopher64's `Exhausted LinkedDeviceHost memory`, and ares crawls at <0.1 fps at the
> same point. ⇒ **The real gameplay keystone is RDP per-tile RENDERING perf, not ROM
> caching.** A dense gameplay room is ~651 tiles × 3 BGs + OBJ = thousands of
> `rdpq_tex_upload`+`texrect` per frame (`Port_N64_RDP_RenderFrame`,
> `port_n64/n64_main.c`); each per-tile TMEM upload is a Vulkan resource in gopher64
> (→ OOM) and the texrect volume crawls ares. The title screen renders fine only
> because its BGs are sparse.
>
> **Re-prioritized next step: RDP per-tile-upload optimization** (batch TMEM:
> load a 4 KB CI4 page = 128 tiles once, draw visible tiles in that page via
> tex-coords; group by palette since the CI4 16-colour bank is per-tile). HARD: the
> earlier `rdpq_tex_multi`/`reuse` attempt hung the RDP (solid-pink, f=0) and was
> reverted — needs a careful manual page+palette batching pass. The ROM-cache plan
> below stays valid as a *hardware* perf nicety but is **not** what's blocking
> gameplay verification on emulator.

# Phase-7 — Fast ROM access (RDRAM caching)

Keystone for the gameplay-gated roadmap. Gameplay is *functionally* reached on N64
(autoplay → `TASK_GAME`, frames complete, not hung) but is impractically slow on
emulator and would be on hardware. This plan turns it playable so the gameplay
items (RDP BG-scroll verify, affine/windows/blend, save, file-select, audio) can
actually be exercised.

## Root cause
- `gRomData = 0xA0000000 | (cart & 0x1FFFFFFF)` — **KSEG1 (uncached) cart**
  (`port_n64/n64_main.c`). Every `gRomData[off]` / `Port_ReadU8/16/32` /
  `Port_ResolveRomData(addr)`-then-deref is an uncached PI-bus access.
- 16 MB ROM > 8 MB RDRAM (Expansion Pak) → cannot blanket-copy the ROM in.

## Already fast — do NOT redo
- `__wrap_memcpy` (n64_main.c) PI-DMAs cart→RDRAM for `memcpy` with a cart src
  (n>16, parity-matched).
- `DmaSet` copy mode → `memcpy` → PI DMA (`gba/macro.h::port_DmaTransfer`).
- LZ77 (`n64_glue.c::Lz77Decompress`) DMAs the compressed cart stream into
  `sLz77DmaBuf`, then decompresses.
- ⇒ Bulk room loads (gfx→VRAM, map decompress) are already DMA-backed.
- Most static tables are compile-time (RAM): `gFrameObjLists`, `gFixedTypeGfxData`,
  `gSpritePtrs`, `gObjectDefinitions`, `gAreaRoomHeaders`. Per-frame reads of these
  are already fast.

## Residual cost (the target)
Frequent **small uncached reads** of the big ~13 MB `gGlobalGfxAndPalettes` blob and
other cart regions via `gRomData[off]` for data not pre-copied (packed-pointer
tables, area/room-header walks, `Port_ResolveRomData` callers that deref a KSEG1
pointer).

## Step 0 — PROFILE FIRST (decides the approach)
Do not cache blind. Build a gameplay binary with `g_n64_autoplay=1` **to a separate
output** (never clobber the title `picori.z64`), then:
1. Instrument `Port_ResolveRomData` + `Port_ReadU8/16/32` (`tmc/port/port_rom.c`)
   with a per-frame access counter + coarse offset histogram (e.g. 64 KB buckets),
   gated `TMC_N64` behind a runtime flag.
2. Run one room; dump reads/frame + top buckets.
3. Time the `GameMain_InitRoom` substate vs steady-state frames.

This answers: is the pause the **one-time room load** (→ targeted DMA, Approach A) or
**per-frame** (→ software cache, Approach B), and **which** ROM regions are hot.

## Approach A — targeted room-load DMA (hot set small & bounded)
If the hot working set fits a budget (≤2–4 MB) and is per-room identifiable, DMA those
ranges into an RDRAM staging buffer at room load and redirect reads of those ranges
to it.

## Approach B — software ROM cache (accesses scattered)
Direct-mapped RDRAM cache, e.g. 256 lines × 4 KB = 1 MB:
- Wrap `Port_ResolveRomData(addr)`: `line = off / L`; on tag miss PI-DMA the L-byte
  line cart→cacheline (**cached KSEG0**), update tag; return `&cacheline[off % L]`.
- Hot reads hit cached RDRAM (fast); cold reads pay one DMA.
- **Caveat:** returned pointers must stay valid for the caller's use — don't evict a
  line mid-use. Fine for the dominant read-one-value pattern; for callers that hold a
  ROM pointer across many reads, pin the line or copy the needed span.

## Order of work
1. Profile (Step 0) → pick A or B from real data.
2. Implement behind `g_n64_rom_cache` (gated `TMC_N64`; PC build byte-identical).
3. Re-run the gameplay binary; confirm fps + room renders identically (diff vs the
   known-good software render).
4. Then live-verify RDP BG scroll (walk Link, watch `BG1HOFS`/`link.x` change).

## Cleanup this unlocks
The 1 MB `gMapData` placeholder + its memcpy are dead weight on N64 (`LoadMapData`
reads src from cart now) — remove once the cache/DMA path lands.

## Verification harness (reuse the title method)
- PC reference: `cd build/pc && TMC_PUBLISH_FRAMEBUFFER=1 TMC_AUTOPLAY=1
  SDL_VIDEODRIVER=dummy ./tmc_pc &`, read `/dev/shm/tmc_framebuffer`
  (24 B header `magic"TMCF"/ver/w/h/frameCount/oamCount`, then 240×160 RGBA8 at
  off 24).
- N64: dump the framebuffer as hex over `debugf` (`[FBR y half]…`), rebuild in
  Python/PIL, diff. ares is slow on uncached cart (~95 s to title); use the FB dump,
  not Wayland window screenshots.
