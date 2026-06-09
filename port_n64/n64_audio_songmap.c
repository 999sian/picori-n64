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
