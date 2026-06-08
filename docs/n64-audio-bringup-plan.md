# N64 Audio Bring-Up Plan

**Status: NOT IMPLEMENTED (game is silent).** The M4A *bookkeeping* ABI is now
correct on N64 (see below); the actual synthesizer + AI output is the remaining
large piece. This doc is the runway for that work — it is not a claim that audio
works.

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

## Available libdragon APIs (verified present)

`$N64_INST/mips64-elf/include/`: `audio.h` (AI double-buffered DAC),
`mixer.h` (multi-channel resampling mixer + `waveform_t` sources),
`samplebuffer.h`. AI output is straightforward; the synth is the work.

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

Recommendation: option 1 (parity + reuse) unless agbplay proves impractical to
compile, then fall back to option 2 PCM-first.

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

## Scope

Substantial — `tmc/docs/n64-port-plan.md` classifies full audio as a multi-month
item. The ABI fix above is the only part landed; the synth + AI output remain.
