/*
 * port_n64/n64_audio_songmap.c — N64 M4A song-data locator (Option-2 synth groundwork).
 *
 * The C song symbols (bgmTitleScreen, ...) are zero stubs on the port; the real
 * M4A song/voicegroup/sample data lives in the cart baserom. This maps a songId
 * to the cart ROM byte offset of its SongHeader (table generated from
 * assets/sounds.json by gen_song_offsets.py) and reads it endian-correct via the
 * cart-safe Port_ReadU* helpers. This is the data foundation the synth builds on;
 * Port_N64_AudioProbeSongs() dumps a few decoded headers for verification.
 */
#include "gba/types.h"
#include "sound.h"      /* song id enum (SFX_NONE, BGM_TITLE_SCREEN, ...) */
#include "port_rom.h"   /* cart-safe Port_ReadU32 (aligned lw + LE) */
#include <stdint.h>

extern unsigned char* gRomData;
extern unsigned int gRomSize;
extern void debugf(const char* fmt, ...);

/* songId -> cart ROM byte offset of the song's SongHeader (0 = no entry). */
const uint32_t kSongRomOffset[] = {
#include "n64_song_offsets.inc"
};
const unsigned kSongRomOffsetCount = (unsigned)(sizeof(kSongRomOffset) / sizeof(kSongRomOffset[0]));

/* Resolve a songId to a readable cart pointer to its SongHeader, or NULL. */
const void* Port_N64_SongHeaderPtr(uint16_t songId) {
    uint32_t off;
    if (songId >= kSongRomOffsetCount) return 0;
    off = kSongRomOffset[songId];
    if (off == 0u || off + 12u > gRomSize) return 0;
    return (const void*)(gRomData + off);
}

/* Resolve a GBA ROM address (0x08xxxxxx) to a readable cart pointer, or NULL. */
static const u8* GbaAddrToCart(u32 gbaAddr) {
    u32 off;
    if (gbaAddr < 0x08000000u) return 0;
    off = gbaAddr - 0x08000000u;
    if (off + 16u > gRomSize) return 0;
    return gRomData + off;
}

/* Bring-up verification: dump decoded SongHeaders for a few songs. Expected for
 * BGM_TITLE_SCREEN (ground truth from baserom): trk=7 tone=089fd5fc part0=08dcc864. */
void Port_N64_AudioProbeSongs(void) {
    static const uint16_t ids[] = { BGM_CASTLE_TOURNAMENT, BGM_VAATI_MOTIF, BGM_TITLE_SCREEN };
    unsigned i;
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        uint16_t id = ids[i];
        const u8* base = (const u8*)Port_N64_SongHeaderPtr(id);
        u32 w0, tone, part0;
        if (base == 0) { debugf("[song] id=%u no/invalid offset\n", id); continue; }
        w0    = Port_ReadU32(base);        /* trackCount|blockCount<<8|priority<<16|reverb<<24 */
        tone  = Port_ReadU32(base + 4);    /* ToneData* (GBA addr, LE) */
        part0 = Port_ReadU32(base + 8);    /* track[0] cmd stream (GBA addr, LE) */
        debugf("[song] id=%u off=%lu trk=%u blk=%u pri=%u rev=%u tone=%08lx part0=%08lx\n",
               id, (unsigned long)kSongRomOffset[id], (unsigned)(w0 & 0xFFu),
               (unsigned)((w0 >> 8) & 0xFFu), (unsigned)((w0 >> 16) & 0xFFu),
               (unsigned)((w0 >> 24) & 0xFFu), (unsigned long)tone, (unsigned long)part0);
    }
}

/* Walk a song's voicegroup (ToneData[128]) for the first DirectSound (PCM) voice
 * and decode its WaveData header — the sample the mixer will play. Verifies the
 * full SongHeader->ToneData->WaveData cart-read chain. Ground truth for
 * BGM_TITLE_SCREEN: prog27 WaveData{type=0 freq=16146432 loopStart=9629 size=16168}. */
void Port_N64_AudioProbeVoice(uint16_t songId) {
    const u8* hdr = (const u8*)Port_N64_SongHeaderPtr(songId);
    const u8* tone;
    unsigned prog;
    if (hdr == 0) { debugf("[voice] id=%u no song\n", songId); return; }
    tone = GbaAddrToCart(Port_ReadU32(hdr + 4));   /* SongHeader.tone (voicegroup) */
    if (tone == 0) { debugf("[voice] id=%u bad voicegroup\n", songId); return; }
    for (prog = 0; prog < 128u; prog++) {
        const u8* td = tone + prog * 12u;          /* ToneData entry */
        u32 type = Port_ReadU32(td) & 0xFFu;       /* type|key<<8|len<<16|pan<<24 */
        u32 wavAddr;
        const u8* wav;
        if (type != 0x00u && type != 0x08u) continue;   /* DirectSound (PCM) only */
        wavAddr = Port_ReadU32(td + 4);            /* WaveData* */
        wav = GbaAddrToCart(wavAddr);
        if (wav == 0) continue;
        debugf("[voice] id=%u prog=%u wav=%08lx WaveData{type=%lu freq=%lu loopStart=%lu size=%lu}\n",
               songId, prog, (unsigned long)wavAddr,
               (unsigned long)(Port_ReadU32(wav) & 0xFFFFu),   /* WaveData.type (u16) */
               (unsigned long)Port_ReadU32(wav + 4),           /* .freq */
               (unsigned long)Port_ReadU32(wav + 8),           /* .loopStart */
               (unsigned long)Port_ReadU32(wav + 12));         /* .size (samples) */
        return;
    }
    debugf("[voice] id=%u no DirectSound voice in 128 progs\n", songId);
}
