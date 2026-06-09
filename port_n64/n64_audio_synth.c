/*
 * port_n64/n64_audio_synth.c — N64 M4A PCM synth (Option-2, WIP / gated).
 *
 * First-cut sequencer + DirectSound (PCM) mixer feeding Port_M4A_Backend_Render
 * -> the libdragon AI DAC (Port_N64_AudioService). Built on the verified pieces:
 * song locator, voicegroup/WaveData reader, command decoder, tick timing.
 *
 * Scope of this first cut (honest): DirectSound voices only (PSG square/wave/
 * noise deferred); nearest-neighbour resample; simple attack/release envelope;
 * tempo + pitch from agbplay's exact constants. Correctness of *sound* (pitch/
 * tempo/timbre) is NOT headlessly verifiable for a fresh synth — needs listening.
 * Gated by g_n64_audio (default off): the shipped ROM is unaffected.
 *
 * Samples are PI-DMA'd from the cart into an RDRAM cache once (per-sample uncached
 * cart reads at the output rate would be far too slow), then mixed from RDRAM.
 */
#include "gba/types.h"
#include "port_rom.h"
#include <stdint.h>
#include <string.h>

extern unsigned char* gRomData;
extern unsigned int gRomSize;
extern const void* Port_N64_SongHeaderPtr(uint16_t songId);

#define SR            32000u
#define MAXTRK        16
#define MAXVOICE      24
#define NCACHE        24
#define SLOT_BYTES    0x10000u   /* 64 KiB per cached sample (truncate larger) */

/* GBA note-length / delay LUT (same 49-entry table as the decoder). */
static const unsigned char kLen[49] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68, 72,
    76, 78, 80, 84, 88, 90, 92, 96
};

/* 2^(n/12) in Q16.16 for n = 0..11. */
static const uint32_t kSemi[12] = {
    65536u, 69433u, 73562u, 77936u, 82570u, 87480u,
    92682u, 98193u, 104032u, 110218u, 116771u, 123712u
};

typedef struct {
    u32 wavOff;            /* cart offset of the WaveData (key) */
    u32 len, loopStart;
    int looped;
    s8* data;              /* RDRAM-cached PCM (points into sCacheMem) */
} SampleSlot;

typedef struct {
    int active;            /* 0 free, 1 sustaining, 2 releasing */
    int psg;               /* 0 = PCM voice, 1 = PSG square */
    SampleSlot* s;
    u32 cur;               /* Q16.16 sample cursor (PCM) */
    u32 step;              /* Q16.16 advance per output sample (PCM) */
    u32 phase, phaseStep;  /* square-wave phase accumulator (PSG, 65536 = 1 period) */
    u32 duty;              /* square duty threshold (PSG) */
    int rem;               /* remaining ticks; <0 = tie (until expiry elsewhere) */
    int env;               /* Q8 envelope 0..256 */
    int vol;               /* combined 0..127 */
    int pan;               /* 0..127, 64 = center */
} Voice;

typedef struct {
    u32 pos;               /* cart ROM offset into the command stream */
    unsigned delay, lastCmd, prog, vol, lastKey;
    int pan, enabled;
    u32 cstk[4]; int cdep; /* PATT/PEND pattern call stack */
} Track;

static u8 sCacheMem[NCACHE][SLOT_BYTES] __attribute__((aligned(16)));
static SampleSlot sCache[NCACHE];
static int sCacheUsed;

static Track sTrk[MAXTRK];
static int sNtrk;
static Voice sVoice[MAXVOICE];
static u32 sVoiceGroup;     /* cart offset of ToneData[] */
static u32 sTickAcc;        /* Q16.16 */
static u32 sSamplesPerTick; /* Q16.16 */
static unsigned sBpm = 150;
static int sActive;

static inline unsigned cart_u8(u32 off) { return Port_N64_CartReadU8Lw(gRomData + off); }
static inline u32 cart_u32(u32 off) { return Port_ReadU32(gRomData + off); }
static inline u32 gba2off(u32 a) { return (a >= 0x08000000u && (a - 0x08000000u) < gRomSize) ? a - 0x08000000u : 0u; }

static void RecalcTempo(void) {
    /* samplesPerTick = SR * 2.5 / bpm  (UNIT_BPM=150, AGB_FPS=60). Q16.16. */
    u64 num = (u64)SR * 5u * 65536u;
    sSamplesPerTick = (u32)(num / (2u * (sBpm ? sBpm : 150u)));
    if (sSamplesPerTick == 0u) sSamplesPerTick = 1u;
}

/* DMA a sample's PCM into the RDRAM cache (once) and return the slot. */
static SampleSlot* GetSample(u32 wavOff) {
    int i;
    SampleSlot* sl;
    u32 size, loop, dataOff, bytes;
    for (i = 0; i < sCacheUsed; i++)
        if (sCache[i].wavOff == wavOff) return &sCache[i];
    if (sCacheUsed >= NCACHE) return 0;        /* cache full: drop new samples */
    loop = cart_u32(wavOff + 8);
    size = cart_u32(wavOff + 12);
    dataOff = wavOff + 16;
    if (size == 0u || dataOff + size > gRomSize) return 0;
    bytes = size; if (bytes > SLOT_BYTES) bytes = SLOT_BYTES;
    sl = &sCache[sCacheUsed];
    sl->data = (s8*)sCacheMem[sCacheUsed];
    memcpy(sl->data, gRomData + dataOff, bytes);  /* __wrap_memcpy -> PI DMA */
    sl->wavOff = wavOff;
    sl->len = bytes;
    sl->loopStart = (loop < bytes) ? loop : 0u;
    sl->looped = (loop < size);
    sCacheUsed++;
    return sl;
}

static void StartVoice(int key, int vel, unsigned prog, int trkVol, int trkPan, int lenTicks) {
    u32 td, type, wavAddr, wavOff, freq;
    SampleSlot* s;
    int vi, octave, sem; u64 step60;
    if (prog > 127u) return;
    td = sVoiceGroup + prog * 12u;
    type = cart_u8(td);
    if (type == 0x01u || type == 0x02u) {            /* PSG square (melody voices) */
        static const u32 dutyT[4] = { 8192u, 16384u, 32768u, 49152u };  /* 12.5/25/50/75% */
        u64 ps;
        for (vi = 0; vi < MAXVOICE; vi++) if (!sVoice[vi].active) break;
        if (vi == MAXVOICE) return;
        /* phaseStep for key 69 (440Hz) @ SR = 440*65536/SR; scale by 2^((key-69)/12). */
        ps = 440u * 65536u / SR;
        octave = (key - 69); sem = octave % 12; octave /= 12;
        if (sem < 0) { sem += 12; octave -= 1; }
        ps = (ps * kSemi[sem]) >> 16;
        if (octave >= 0) ps <<= octave; else ps >>= (-octave);
        if (ps == 0) ps = 1;
        sVoice[vi].active = 1;
        sVoice[vi].psg = 1;
        sVoice[vi].phase = 0;
        sVoice[vi].phaseStep = (u32)ps;
        sVoice[vi].duty = dutyT[cart_u8(td + 4) & 3u];
        sVoice[vi].rem = lenTicks;
        sVoice[vi].env = 0;
        sVoice[vi].vol = (vel * trkVol) / 127;
        sVoice[vi].pan = trkPan;
        return;
    }
    if (type != 0x00u && type != 0x08u) return;      /* otherwise only PCM (DirectSound); wave/noise WIP */
    wavAddr = cart_u32(td + 4);
    wavOff = gba2off(wavAddr);
    if (wavOff == 0u) return;
    freq = cart_u32(wavOff + 4);
    s = GetSample(wavOff);
    if (s == 0) return;
    for (vi = 0; vi < MAXVOICE; vi++) if (!sVoice[vi].active) break;
    if (vi == MAXVOICE) return;                      /* voice steal: skip (WIP) */
    sVoice[vi].psg = 0;
    /* step = (freq/1024/SR) * 2^((key-60)/12), Q16.16.  step60 = freq*64/SR. */
    step60 = ((u64)freq * 64u) / SR;
    octave = (key - 60); sem = octave % 12; octave /= 12;
    if (sem < 0) { sem += 12; octave -= 1; }
    step60 = (step60 * kSemi[sem]) >> 16;
    if (octave >= 0) step60 <<= octave; else step60 >>= (-octave);
    if (step60 == 0) step60 = 1;
    sVoice[vi].active = 1;
    sVoice[vi].s = s;
    sVoice[vi].cur = 0;
    sVoice[vi].step = (step60 > 0x3FFFFFFFu) ? 0x3FFFFFFFu : (u32)step60;
    sVoice[vi].rem = lenTicks;
    sVoice[vi].env = 0;
    sVoice[vi].vol = (vel * trkVol) / 127;
    sVoice[vi].pan = trkPan;
}

static unsigned char CmdArgc(unsigned cmd) {
    switch (cmd) {
        case 0xB1: case 0xB4: return 0;
        case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xC8: return 1;
        case 0xB2: case 0xB3: return 4;
        case 0xB5: return 5;
        default: return 0xFF;
    }
}

/* Run one MP2K tick for a track: expire-tick its notes happens globally; here we
 * advance the command stream when its delay reaches 0 and start voices. */
static void TrackTick(Track* t) {
    if (!t->enabled) return;
    while (t->delay == 0u) {
        unsigned cmd = cart_u8(t->pos);
        if (cmd < 0x80u) { cmd = t->lastCmd; if (cmd < 0x80u) { t->enabled = 0; return; } }
        else { t->pos++; if (cmd >= 0xBDu) t->lastCmd = cmd; }
        if (cmd >= 0xCFu) {                          /* note */
            int len = (int)kLen[cmd - 0xCFu]; int key = -1, vel = 127;
            if (cart_u8(t->pos) < 0x80u) {
                key = (int)cart_u8(t->pos++);
                if (cart_u8(t->pos) < 0x80u) {
                    vel = (int)cart_u8(t->pos++);
                    if (cart_u8(t->pos) < 0x80u) len += (int)cart_u8(t->pos++);
                }
            }
            if (key < 0) key = (int)t->lastKey; else t->lastKey = (unsigned)key;
            if (len > 0) StartVoice(key, vel, t->prog, (int)t->vol, t->pan, len);
        } else if (cmd >= 0xB1u) {                   /* state command */
            unsigned n = CmdArgc(cmd);
            if (cmd == 0xB2u) {                      /* GOTO: follow the loop pointer */
                u32 tgt = gba2off(cart_u32(t->pos));
                if (tgt == 0u) { t->enabled = 0; return; }
                t->pos = tgt;
                continue;
            }
            if (cmd == 0xB3u) {                      /* PATT: call a pattern */
                u32 tgt = gba2off(cart_u32(t->pos));
                if (tgt == 0u || t->cdep >= 4) { t->enabled = 0; return; }
                t->cstk[t->cdep++] = t->pos + 4u;    /* return after the 4-byte pointer */
                t->pos = tgt;
                continue;
            }
            if (cmd == 0xB4u) {                      /* PEND: return from pattern */
                if (t->cdep > 0) t->pos = t->cstk[--t->cdep];
                continue;
            }
            if (n == 0xFFu || cmd == 0xB1u) { t->enabled = 0; return; }   /* FINE / unhandled */
            if (cmd == 0xBB) sBpm = cart_u8(t->pos) * 2u, RecalcTempo();
            else if (cmd == 0xBD) t->prog = cart_u8(t->pos);
            else if (cmd == 0xBE) t->vol = cart_u8(t->pos);
            else if (cmd == 0xBF) t->pan = (int)cart_u8(t->pos);
            t->pos += n;
        } else {                                     /* wait */
            t->delay = kLen[cmd - 0x80u];
        }
    }
    t->delay--;
}

static void DoTick(void) {
    int i;
    /* count down note durations; release expired notes */
    for (i = 0; i < MAXVOICE; i++) {
        if (sVoice[i].active == 1 && sVoice[i].rem > 0) {
            if (--sVoice[i].rem == 0) sVoice[i].active = 2;   /* -> release */
        }
    }
    {
        int anyEnabled = 0, anyVoice = 0;
        for (i = 0; i < sNtrk; i++) { TrackTick(&sTrk[i]); if (sTrk[i].enabled) anyEnabled = 1; }
        for (i = 0; i < MAXVOICE; i++) if (sVoice[i].active) { anyVoice = 1; break; }
        if (!anyEnabled && !anyVoice) sActive = 0;   /* song finished -> allow the game to replay it */
    }
}

static int sCurSong = -1;

void Port_N64_SynthStart(uint16_t songId) {
    const u8* hdr;
    int i, n;
    if (songId == 0u || songId >= 100u) return;  /* synth drives BGM (1..99) only; ignore SFX/silence so they don't clobber */
    if (sActive && sCurSong == (int)songId) return;  /* playing this song: don't restart (skip continue calls) */
    sCurSong = (int)songId;
    sActive = 0;
    memset(sVoice, 0, sizeof(sVoice));
    sCacheUsed = 0;                 /* WIP: flush sample cache per song */
    if (songId == 0) return;        /* sfxNone / silence */
    hdr = (const u8*)Port_N64_SongHeaderPtr(songId);
    if (hdr == 0) return;
    n = (int)cart_u8((u32)((const u8*)hdr - gRomData));   /* trackCount */
    if (n <= 0) return; if (n > MAXTRK) n = MAXTRK;
    sVoiceGroup = gba2off(cart_u32((u32)((const u8*)hdr - gRomData) + 4));
    if (sVoiceGroup == 0u) return;
    for (i = 0; i < n; i++) {
        u32 part = cart_u32((u32)((const u8*)hdr - gRomData) + 8u + (u32)i * 4u);
        u32 off = gba2off(part);
        memset(&sTrk[i], 0, sizeof(sTrk[i]));
        sTrk[i].pos = off; sTrk[i].vol = 127; sTrk[i].pan = 64; sTrk[i].lastKey = 60;
        sTrk[i].enabled = (off != 0u);
    }
    sNtrk = n;
    sBpm = 150; RecalcTempo();
    sTickAcc = 0;
    sActive = 1;
}

void Port_N64_SynthStop(void) { sActive = 0; }

/* Render `frames` stereo Q0 int16 samples (interleaved L,R). */
void Port_N64_SynthRender(int16_t* out, uint32_t frames, int mute) {
    uint32_t f; int i;
    if (!sActive || mute) { memset(out, 0, (size_t)frames * 2u * sizeof(int16_t)); return; }
    for (f = 0; f < frames; f++) {
        int accL = 0, accR = 0;
        /* advance the sequencer tick clock */
        sTickAcc += 65536u;
        while (sTickAcc >= sSamplesPerTick) { sTickAcc -= sSamplesPerTick; DoTick(); }
        for (i = 0; i < MAXVOICE; i++) {
            Voice* v = &sVoice[i];
            int smp, e, val, l, r;
            if (!v->active) continue;
            /* envelope: quick attack, hold, quick release */
            if (v->active == 1) { if (v->env < 256) { v->env += 8; if (v->env > 256) v->env = 256; } }
            else { v->env -= 4; if (v->env <= 0) { v->active = 0; continue; } }
            if (v->psg) {                            /* PSG square (melody) */
                smp = ((v->phase & 0xFFFFu) < v->duty) ? 48 : -48;
                v->phase += v->phaseStep;
            } else {                                 /* PCM (DirectSound) */
                u32 idx = v->cur >> 16;
                if (idx >= v->s->len) {
                    if (v->s->looped && v->s->len > v->s->loopStart) {
                        u32 ll = v->s->len - v->s->loopStart;
                        idx = v->s->loopStart + ((idx - v->s->loopStart) % ll);
                        v->cur = (idx << 16) | (v->cur & 0xFFFFu);
                    } else { v->active = 0; continue; }
                }
                smp = v->s->data[idx];               /* s8 -128..127 */
                v->cur += v->step;
            }
            e = (smp * v->env) >> 8;              /* envelope */
            val = (e * v->vol) >> 7;              /* track/vel volume (0..127) */
            l = (val * (127 - v->pan)) >> 7;     /* pan: 0..127, 64 center */
            r = (val * v->pan) >> 7;
            accL += l << 6;                        /* s8 -> ~int16 headroom */
            accR += r << 6;
        }
        if (accL > 32767) accL = 32767; else if (accL < -32768) accL = -32768;
        if (accR > 32767) accR = 32767; else if (accR < -32768) accR = -32768;
        out[2 * f] = (int16_t)accL;
        out[2 * f + 1] = (int16_t)accR;
    }
}
