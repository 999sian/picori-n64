# N64 Audio Bring-Up Plan

**Status: AI output path landed + verified; synth NOT implemented (game is
silent).** The M4A *bookkeeping* ABI is correct and the **libdragon AI DAC output
service is wired and proven end-to-end** (gated, default off). The remaining large
piece is the actual M4A *synthesizer* — until it lands, the output is silence.
This doc is the runway for that work; it is not a claim that game audio plays.

## Current state (2026-06-08)

- The engine calls the normal `src/sound.c` / `gba/m4a.h` API.
- `tmc/port/port_m4a_stubs.c` **is compiled on N64** (in `build.sh` `PORT_REL`).
  It maintains the GBA-facing `gMusicPlayers[]` / status bookkeeping and forwards
  to `Port_M4A_Backend_*` hooks.
- The real synth has **two** non-compiled sources on N64:
  - `tmc/src/gba/m4a.c` (the GBA sequencer/mixer) — excluded.
  - `tmc/port/port_m4a_backend.cpp` (the PC behavioral synth, agbplay_core) —
    excluded (C++, agbplay deps).
- `port_n64/n64_glue.c` defines the `Port_M4A_Backend_*` hooks. **Fixed this round:**
  the stubs previously had wrong signatures vs `tmc/port/port_m4a_backend.h`
  (e.g. `StartSongById` was `void f(u32)` but is called as `bool f(u8,u16)`), so
  `port_m4a_stubs.c` read a garbage return value and could spuriously `MPlayStop`.
  The glue now `#include`s `port_m4a_backend.h` (compile-time signature check) and
  implements the full hook set as a consistent "nothing playing" backend
  (`StartSongById`/`IsPlayerActive` → false). Result: bookkeeping is correct and
  hang-safe; output is still silence.
- **AI output service landed + verified.** `port_n64/n64_main.c::Port_N64_AudioService`
  drives the libdragon AI DAC from the per-frame VBlank hook (`audio_init(32000,4)`
  + `audio_write_begin/_end`), calling `Port_M4A_Backend_Render` per buffer. Gated
  by `g_n64_audio` / `g_n64_audio_selftest` (both **default off** → shipped ROM
  unaffected). Verified on ares: with the self-test tone on, the `aud=` telemetry
  counter rises monotonically (~1.7 buffers/frame, no underrun/crash), proving the
  AI is actively draining buffers (a 250 Hz square is clocked to the DAC). This is
  the exact buffer the synth will fill — only `Port_M4A_Backend_Render` (currently
  silence) needs replacing with real samples.

## Available libdragon APIs (verified present)

`$N64_INST/mips64-elf/include/`: `audio.h` (AI double-buffered DAC),
`mixer.h` (multi-channel resampling mixer + `waveform_t` sources),
`samplebuffer.h`. AI output is straightforward; the synth is the work.

## Where the song data actually is (verified — important)

The C song symbols are **zero stubs**, not real data — a synth that reads
`gSongTable[id].header` would read zeros:
- `tmc/src/sound.c::gSongTable[]` is a real native array of
  `Song{ const SongHeader* header; u16 musicPlayerIndex; u16 me }`, but each
  `header` points at a placeholder: `tmc/port/port_linked_stubs.c:349-363`
  (`u8 bgmVaatiMotif[0x10]` …, 16 zero bytes) and `tmc/port/data_stubs_autogen.c`
  (`const u8 sfxNone[1] = {0}`).
- The real song/voicegroup/sample data is `.incbin` GBA assembly under
  `tmc/data/sound/` (`sounds.s` → `sounds/*.s`), which is **not compiled** into
  the C/N64 build at all.

How the PC backend actually plays (the reference, `tmc/port/port_m4a_backend.cpp`):
it reads songs **from the cart ROM by byte offset**, not from the C symbols.
`SongIdToRomPosLocked(songId)` returns a ROM offset from a loaded song map; an
agbplay `Rom` wraps `gRomData`, and `MP2KContext` (agbplay's MP2K = the full GBA
M4A sequencer + mixer, `songTableInfo.pos = POS_AUTO` auto-detects the MP2K song
table in the ROM) renders from there via `m4aMPlayStart(playerIndex, songPos)`.

**Consequences for the N64 synth:**
1. Read all song/voicegroup/sample data from the **cart** (`gRomData`, KSEG1),
   using the cart-read discipline (`Port_ReadU*` / PI-DMA staging, little-endian).
   Ignore the C `bgm*`/`sfx*` stubs entirely.
2. Obtain a `songId → ROM offset` map (port the PC `LoadSongMap`, or auto-detect
   the MP2K song table in `gRomData` as agbplay's `POS_AUTO` does).
3. Implement the MP2K sequencer + mixer (this is the bulk — see options below).

Note: `tmc/libs/agbplay_core/` is a submodule **directory present but not checked
out** on N64 (`build.sh` inits only `tmc` + `tmc/libs/ViruaPPU`). Option 1 must
first init + vendor it for `mips64-elf-g++`.

## Implementation options

1. **Port agbplay_core (the PC backend) to N64.** Pros: behavioral parity with the
   verified PC port; reuse `Port_M4A_Backend_Render(int16*, frames, mute)`. Cons:
   C++ + float-heavy on the VR4300 (FPU works but slow), agbplay's own structure
   to compile under libdragon `mips64-elf-g++`, and ROM access must route through
   the cart bridge (`Port_ReadU*`, PI-DMA staging) and be endian-correct (song
   data is little-endian — same `lw`/bswap discipline as the rest of the port).
2. **Write a fresh minimal M4A sequencer + mixer.** Start with DirectSound (PCM)
   channels only (most music + sampled SFX), defer PSG square/wave/noise. Smaller
   surface, but reimplements sequencing (note on/off, tempo, ADSR, pitch, the
   `voicegroup`/`SongHeader` walk) — i.e. most of what option 1 already has.

## Toolchain finding (verified — flips the recommendation)

**Option 1 does NOT compile on the stock toolchain.** Test-compiling
`agbplay_core/MP2KContext.cpp` with `mips64-elf-g++ -std=gnu++20` fails at
`#include <cstdint>`: the libdragon GCC 15.2.0 install ships `libstdc++.a` but
**no C++ standard headers**. `mips64-elf-g++ -E -v` lists only
`lib/gcc/mips64-elf/15.2.0/include`, `…/include-fixed`, and `mips64-elf/include`
(newlib C) — there is no `c++/` header dir, so `<cstdint>`/`<vector>`/`<span>`/
`<memory>` are all unavailable. agbplay is heavy STL (`std::vector`, `std::span`,
`std::unique_ptr`, `<algorithm>`, `<cmath>`), so it cannot build as-is.

Two ways to unblock Option 1, both heavy: (a) rebuild the libdragon toolchain
with hosted libstdc++ headers (`--enable-libstdcxx` + target headers) — a
build-environment change affecting everyone; or (b) strip all STL out of agbplay
(fixed buffers instead of vector/span/unique_ptr) — effectively a rewrite, i.e.
Option 2 in disguise.

**Revised recommendation: Option 2** — a fresh STL-free C M4A sequencer + PCM
mixer that lives in `port_n64/` (or as a `TMC_N64` C TU), keeping the stock
toolchain. Reuse agbplay only as a *reference* for behavior (sequencer command
semantics, ADSR, resampling, mixing) — it is now checked out for reading. Pursue
Option 1 only if hardware-exact parity is later deemed worth a toolchain rebuild.

## Option 1 porting surface (read from agbplay_core source — definitive)

`tmc/libs/agbplay_core/` is now checked out. The C++ engine to port for
`mips64-elf-g++`:
- **Engine:** `MP2KContext` (+ `MP2KPlayer`, `MP2KTrack`, `MP2KChn`/`MP2KChnPCM`/
  `MP2KChnPSG`, `SequenceReader`, `SoundMixer`, `Resampler`, `ReverbEffect`,
  `LoudnessCalculator`, `CGBPatterns`). Drive per frame with
  `MP2KContext::m4aSoundMain()` then drain `masterAudioBuffer` into the AI service.
- **Start:** `m4aMPlayStart(playerIndex, songHeaderRomOffset)` — caller supplies the
  song's **ROM byte offset** (not a pointer).
- **ROM access:** `MP2KContext` reads through agbplay `Rom` (`Rom.cpp`), which
  assumes an in-RAM `std::span<uint8_t>` over the whole ROM. On N64 `gRomData` is
  **uncached KSEG1 cart, 16 MB > 8 MB RDRAM** — so `Rom` must be adapted to read
  via the cart bridge (PI-DMA song/sample regions to RDRAM on demand; bytes are
  already correct since agbplay reads byte-wise + assembles LE itself, but the
  per-byte cart `lbu` hazard means routing reads through aligned `lw`/DMA).
- **Song map (NOT agbplay auto-detect):** the PC backend's
  `port_m4a_backend.cpp::LoadSongMapLocked` builds `songHeaderOffsets[songId]` from
  **`assets/sounds.json`** (match `Port_GetSongLabel(songId)` →
  `startOffset + headerOffset`). N64 needs that JSON available (embed in the DFS or
  precompute a C `songId→offset` table at build time); `SongTableInfo::POS_AUTO`
  runtime detection is unused on this path.
- **Render wiring:** already in place — `Port_M4A_Backend_Render` (n64_glue.c) is the
  socket; `Port_N64_AudioService` (n64_main.c) drains it to the AI DAC.

First verifiable brick (next session): compile the agbplay TUs for mips64-elf,
stand up a `Rom` over a DMA'd song region, and dump the first decoded
`SongHeader` for one songId (trackCount/voicegroup sane) before wiring `Render`.

## Output path (either option)

- `audio_init(sampleRate, buffers)` once at boot (`n64_main.c`).
- Per emulated GBA frame in `Port_N64_VBlank`: when an AI buffer is free,
  `Port_M4A_Backend_Render(buf, frames, mute)` and `audio_write(buf)`. Frame count
  = `sampleRate / 60`. Respect `Port_AudioMute_ShouldSuppress`.

## Verification (headless, no listening required)

Reuse the cross-platform numeric-parity method already proven for the framebuffer:
1. PC ref: drive a known song through the PC `Port_M4A_Backend_Render` and dump the
   int16 PCM buffer.
2. N64: dump the same `Render` output via `debugf` hex.
3. `cmp`/diff the PCM numerically. The base synth is integer-deterministic in the
   GBA-accurate path, so a matching song+frame should match sample-for-sample
   (option 1) or be perceptually equivalent (option 2 PCM-only).

## Option 2 progress (landed + verified)

The **song-data locator** (the Option-2 foundation) is implemented and verified:
- `port_n64/gen_song_offsets.py` reads `assets/sounds.json` + `port_song_midi_table.inc`
  and emits `port_n64/n64_song_offsets.inc` — a `songId → cart ROM byte offset`
  table (`start + headerOffset`), using the `sound.h` enum designators so the C
  compiler resolves indices. 507 songs mapped.
- `port_n64/n64_audio_songmap.c` provides `Port_N64_SongHeaderPtr(songId)` (resolves
  to a cart pointer) and reads the `SongHeader` endian-correct via the cart-safe
  `Port_ReadU32`. `Port_N64_AudioProbeSongs()` (gated `g_n64_audio_probe`, default
  off) dumps decoded headers.
- **Verified on ares** vs baserom ground truth: `BGM_TITLE_SCREEN` →
  `trk=7 tone=0x089fd5fc part0=0x08dcc864` (exact match); ids 1/2 also sane.

The **voicegroup + sample reader** is also landed and verified:
`Port_N64_AudioProbeVoice(songId)` walks the `ToneData[128]` voicegroup
(`SongHeader.tone`) for the first DirectSound (PCM) voice and decodes its
`WaveData{type,freq,loopStart,size}` — all via cart-safe reads + `GbaAddrToCart`.
On ares for `BGM_TITLE_SCREEN`: `prog27 wav=0x08a16610
WaveData{type=0 freq=16146432 loopStart=9629 size=16168}` — exact match to the
baserom. So the full `SongHeader → ToneData → WaveData` cart-read chain (the
mixer's entire input path) is proven. (Title voicegroup is 109 PSG + 17
DirectSound voices — a PCM-only first cut will be missing the PSG melody.)

The **MP2K command-stream decoder** (the sequencer's front half) is landed and
rigorously verified: `Port_N64_AudioProbeTrack(songId)` decodes a track's
`part[]` command stream with the exact agbplay `SequenceReader` semantics —
running status (`lastCmd`, repeatable `≥0xBD`), note-arg parsing (base len from
the shared 49-entry GBA length LUT, then up to 3 optional `<0x80` bytes =
key/vel/len-extend), waits (`0x80–0xB0`), and state commands (`0xB1–0xCE`, with
per-command arg counts). **Verified by parity**: a Python re-implementation of
the same loop decoded `BGM_TITLE_SCREEN` track[0] to an event list, and the N64
decoder reproduced all 16 events **exactly** (`KEYSH=0, TEMPO=250, VOICE=62,
VOL=48, WAIT72, TEMPO=60, …` plus running-status notes with correct variable
arg counts).

The **sequencer back half (timing/note-dispatch)** is also landed and verified:
`Port_N64_AudioStepTrack(songId)` steps a track over MP2K ticks — track-delay
accumulation + per-note length expiry (matching agbplay `TrackMain`/
`TickTrackNotes`) — and emits the note ON/OFF timeline. **Verified by parity**: a
Python tick-by-tick re-implementation and the N64 stepper agree exactly on
`BGM_TITLE_SCREEN` track[0] (`ON58@96 OFF58@105 ON53@120 OFF53@144 ON58@144 …`,
correct durations, simultaneous OFF/ON at shared ticks). So the **complete MP2K
sequencer (parse + timing) is proven** on N64; only the audio-rate mixer remains.

## Scope / remaining

Substantial — `tmc/docs/n64-port-plan.md` classifies full audio as multi-month.
Landed + verified: M4A bookkeeping ABI fix; AI output service; song-data locator;
voicegroup/sample reader; **complete MP2K sequencer (command parse + tick
timing/note-dispatch)**. **Remaining: the audio-rate mixer DSP only** — map the
song tempo to samples-per-tick, and for each active note resample its voicegroup
`WaveData` PCM at the note pitch with an ADSR envelope, mixing all voices (+ the
PSG square/wave/noise voices) into the int16 buffer `Port_M4A_Backend_Render`
hands the AI DAC. Use `tmc/libs/agbplay_core` (`SoundMixer`/`Resampler`/
`MP2KChnPCM`) as the behavior reference.

**Verification boundary (why the mixer is the last, user-in-the-loop step):**
everything up to and including the sequencer was verified headlessly by exact
parity against a Python re-implementation or baserom ground truth. The mixer is
different — a *fresh* synth's PCM won't be sample-exact to agbplay, so headless
checks bottom out at "non-silent, in-range, deterministic, responds to the
verified note events." Confirming it actually *sounds right* (pitch, tempo,
timbre) needs listening — so the mixer ships gated (`g_n64_audio`, default off)
as an explicit WIP for on-device/emulator listen-testing.