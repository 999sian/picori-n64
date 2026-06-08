/*
 * port/n64/n64_glue.c — N64 port glue.
 *
 * Provides the symbols the N64 build doesn't get from the portable port/ files
 * (the SDL/PC-only files are excluded). Three groups:
 *   1. GBA BIOS calls   — real implementations (Div/Sqrt/CpuSet/LZ77/…).
 *   2. ROM/asset data   — placeholders the Phase-3 boot loader will fill.
 *   3. Subsystem stubs  — audio/EEPROM/asset-loader/input/TTS, link-only for
 *                         now; real N64 impls land in Phases 3-5.
 *
 * Built with -DTMC_N64 (so u32/s32 match the engine) and -DPC_PORT (bridge).
 */
#include "gba/types.h"
#include "gba/syscall.h"
#include "gba/eeprom.h"
#include "port_gba_mem.h"   /* port_resolve_addr: GBA addr -> gVram/gEwram/... */
#include "port_m4a_backend.h" /* canonical Port_M4A_Backend_* signatures */
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>

#ifdef TMC_N64
/* libdragon PI DMA + cache ops (declared, not via <libdragon.h>, to avoid type
 * clashes with the game headers above). */
extern void dma_read(void* ram_address, unsigned long pi_address, unsigned long len);
extern void sram_init(void);
extern int sram_detect(void);
extern int sram_read(void* dst, size_t offset, size_t len);
extern int sram_write(const void* src, size_t offset, size_t len);
extern void debugf(const char* fmt, ...);
extern void data_cache_hit_writeback_invalidate(volatile void* addr, unsigned long length);
static u8 sLz77DmaBuf[0x18000] __attribute__((aligned(16))); /* 96KB cart->RDRAM stage */
static inline u8 n64_cart_read_u8_lw(const void* data) {
    uintptr_t addr = (uintptr_t)data;
    uint32_t word = *(const volatile uint32_t*)(addr & ~(uintptr_t)3u);
    return (uint8_t)(word >> (8u * (3u - (uint32_t)(addr & 3u))));
}

static inline u32 n64_lz77_header_size(const u8* s) {
    uint32_t phys = (uint32_t)((uintptr_t)s & 0x1FFFFFFFu);
    if (phys >= 0x10000000u && phys < 0x1FC00000u) {
        return (u32)n64_cart_read_u8_lw(s + 1) | ((u32)n64_cart_read_u8_lw(s + 2) << 8) |
               ((u32)n64_cart_read_u8_lw(s + 3) << 16);
    }
    return (u32)s[1] | ((u32)s[2] << 8) | ((u32)s[3] << 16);
}

#endif

/* src/main.c (under PC_PORT) arms a soft-reset longjmp target defined in
 * port_bios.c — which is excluded from the N64 build. Provide the storage here;
 * on N64 the reset path is never taken (setjmp returns 0, flag is just armed). */
jmp_buf gPortSoftResetJmp;
int gPortSoftResetArmed = 0;

/* Diagnostic: last stray GBA address seen by gba_MemPtr (recorded instead of
 * aborting on N64), surfaced on-screen by Port_N64_VBlank. */
volatile uint32_t g_n64_last_bad_addr = 0;
volatile uint32_t g_n64_bad_count = 0;
volatile uint32_t g_n64_last_abort_ra = 0;
volatile uint32_t g_n64_abort_count = 0;

/* N64 bring-up: make assert/abort non-fatal. The engine/port hit PC-port safety
 * traps (missing-asset, NULL guards, FatalRomError) that halt into libdragon's
 * inspector; on real GBA these paths don't exist. Record the caller (shown on
 * screen) and return so the engine limps forward to the render loop. */
void abort(void) {
    g_n64_last_abort_ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
    g_n64_abort_count++;
    while (0) { } /* non-fatal: returns (noreturn override) */
}

/* Non-fatal C-assert handler: installed into libdragon's __assert_func_ptr by
 * n64_main so failed asserts record their expr/file/line (shown on-screen)
 * and return instead of trapping into the inspector. */
const char* g_n64_assert_expr = 0;
const char* g_n64_assert_file = 0;
volatile int g_n64_assert_line = 0;
volatile uint32_t g_n64_assert_count = 0;
void n64_assert_handler(const char* file, int line, const char* func, const char* expr) {
    (void)func;
    g_n64_assert_file = file;
    g_n64_assert_line = line;
    g_n64_assert_expr = expr;
    g_n64_assert_count++;
}

/* libdragon assertf()/ASSERT() call __inspector_assertion directly (bypassing
 * __assert_func_ptr). The link wraps it (--wrap=__inspector_assertion) here so
 * those are non-fatal too; record the failed expr (shown on-screen) and return. */
void __wrap___inspector_assertion(const char* failedexpr, const char* msg, va_list args) {
    (void)args;
    g_n64_assert_expr = failedexpr ? failedexpr : msg;
    g_n64_assert_count++;
}

/* ===== 1. GBA BIOS ===================================================== */

s32 Div(s32 num, s32 denom) { return denom ? num / denom : 0; }

u64 DivAndModCombined(s32 num, s32 denom) {
    if (denom == 0) return 0;
    s32 q = num / denom, r = num % denom;
    /* SplitDWord: LO = quotient, HI = remainder (endian-aware union keeps
     * LO at the low 32 bits on big-endian too). */
    return ((u64)(u32)r << 32) | (u32)q;
}

u16 Sqrt(u32 n) {
    u32 r = 0, b = 1u << 30;
    while (b > n) b >>= 2;
    while (b) {
        if (n >= r + b) { n -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (u16)r;
}

void CpuSet(const void* src, void* dest, u32 control) {
    u32 count = control & 0x1FFFFF;
    int fixed = (control >> 24) & 1;
    int is32  = (control >> 26) & 1;
    if (is32) {
        const u32* s = (const u32*)src; u32* d = (u32*)dest;
        for (u32 i = 0; i < count; i++) d[i] = fixed ? s[0] : s[i];
    } else {
        const u16* s = (const u16*)src; u16* d = (u16*)dest;
        for (u32 i = 0; i < count; i++) d[i] = fixed ? s[0] : s[i];
    }
}

/* GBA LZ77 (type 0x10): 4-byte header (size in bytes 1..3), then flag-byte
 * groups of 8; bit set = backref (len=(b0>>4)+3, disp=((b0&0xF)<<8|b1)+1). */
static void Lz77Decompress(const void* src, void* dest) {
    const u8* s = (const u8*)src;
#ifdef TMC_N64
    /* An uncached cart src means the decompressor reads it one byte at a time over
     * the PI bus — thousands of slow reads that stall every room load (and hurt
     * real hardware). DMA the whole compressed stream into RDRAM once, decompress
     * from there. (Falls through to the direct path for the rare oversized blob.) */
    {
        uint32_t phys = (uint32_t)((uintptr_t)src & 0x1FFFFFFFu);
        if (phys >= 0x10000000u && phys < 0x1FC00000u) {
            u32 dsz  = n64_lz77_header_size(s); /* decompressed size */
            u32 need = 4u + dsz + (dsz >> 3) + 32u;  /* header + worst-case LZ77 + pad */
            if (need <= sizeof sLz77DmaBuf) {
                need = (need + 15u) & ~15u;
                data_cache_hit_writeback_invalidate(sLz77DmaBuf, need);
                dma_read(sLz77DmaBuf, phys, need);
                s = sLz77DmaBuf;
            }
        }
    }
#endif
    u8* d = (u8*)dest;
    u32 size = n64_lz77_header_size(s);
    s += 4;
    u32 done = 0;
    while (done < size) {
        u8 flags = *s++;
        for (int i = 0; i < 8 && done < size; i++) {
            if (flags & 0x80) {
                u8 b0 = *s++, b1 = *s++;
                u32 len = (b0 >> 4) + 3;
                u32 disp = (((u32)(b0 & 0xF) << 8) | b1) + 1;
                for (u32 j = 0; j < len && done < size; j++) { d[done] = d[done - disp]; done++; }
            } else {
                d[done++] = *s++;
            }
            flags <<= 1;
        }
    }
}
/* Resolve only literal GBA addresses (< 0x80000000). Already-native pointers —
 * KSEG0 RAM like gMapData/gEwram (LoadMapData passes these via Port_ResolveEwramPtr
 * and &gMapData) and KSEG1 cart pointers — pass straight through; Lz77Decompress
 * itself DMAs a cart src. WITHOUT this guard, LoadMapData's native dest/src were
 * double-resolved into garbage so room maps/tilesets decompressed nowhere (black
 * room); literal GBA-address callers (e.g. VRAM 0x06000000) still resolve. */
static const void* n64_lz77_addr(const void* p) {
    uintptr_t a = (uintptr_t)p;
    return (a >= 0x80000000u) ? p : (const void*)port_resolve_addr(a);
}
void LZ77UnCompWram(const void* src, void* dest) {
    Lz77Decompress(n64_lz77_addr(src), (void*)n64_lz77_addr(dest));
}
void LZ77UnCompVram(const void* src, void* dest) {
    Lz77Decompress(n64_lz77_addr(src), (void*)n64_lz77_addr(dest));
}

void Port_N64_VBlank(void); /* defined in n64_main.c: pad pump + display pacing */
extern void VBlankIntr(void); /* engine VBlank handler (src/interrupts.c) */
void VBlankIntrWait(void) {
    /* Mirror the PC port (port_bios.c:VBlankIntrWait): present the current GBA
     * frame and pump input first, then run the engine's VBlank handler. The
     * handler sets gMain.interruptFlag (WaitForNextFrame spins on it — without
     * this the engine hangs after the first frame) and applies the shadow
     * display state via DispCtrlSet + VBlank DMA. */
    Port_N64_VBlank();
    VBlankIntr();
}
void SoftReset(u32 resetFlags) { (void)resetFlags; }
void RegisterRamReset(u32 resetFlags) { (void)resetFlags; }
/* BgAffineSet (SWI 0x0E) — scale-only (matches the PC port_bios.c impl; the title
 * sword passes alpha=0). Was a no-op, so the sword's BG2 affine matrix was never
 * computed and the title swoop-in animation stalled. */
void BgAffineSet(struct BgAffineSrcData* src, struct BgAffineDstData* dest, s32 count) {
    for (s32 i = 0; i < count; i++) {
        dest[i].pa = src[i].sx;
        dest[i].pb = 0;
        dest[i].pc = 0;
        dest[i].pd = src[i].sy;
        dest[i].dx = src[i].texX - src[i].scrX * src[i].sx;
        dest[i].dy = src[i].texY - src[i].scrY * src[i].sy;
    }
}
void ObjAffineSet(struct ObjAffineSrcData* src, void* dest, s32 count, s32 offset) { (void)src; (void)dest; (void)count; (void)offset; }

/* ===== 2. ROM / asset data (filled by the Phase-3 boot loader) ========= */

void* Port_GetSpriteAnimationData(u32 a) { (void)a; return 0; }
void* Port_GetMapAssetDataByIndex(u32 a) { (void)a; return 0; }
/* #N64: real check (the asset-loader impl is excluded). A RoomHeader (0xA bytes)
 * is readable if it lies fully inside the cart ROM image. Returning 0 here made
 * GetAreaRoomHeaderTable reject every area -> no room ever loaded (black room). */
int Port_IsRoomHeaderPtrReadable(const void* ptr) {
    extern unsigned char* gRomData; extern unsigned int gRomSize;
    const unsigned char* p = (const unsigned char*)ptr;
    if (p == 0 || gRomData == 0) return 0;
    return (p >= gRomData && p + 0xA <= gRomData + gRomSize) ? 1 : 0;
}
int Port_IsLoadedAssetBytes(const void* p, u32 n) { (void)p; (void)n; return 0; }
int Port_LoadPaletteGroupFromAssets(u32 a) { (void)a; return 0; }
int Port_LoadGfxGroupFromAssets(u32 a) { (void)a; return 0; }
void Port_LogTextLookup(u32 lang, u32 text) { (void)lang; (void)text; }
void Port_DumpAssetEnvironment(void) {}
/* Asset-pipeline predicates referenced by port_rom.c (real impls in the
 * excluded port_asset_loader.cpp). Return "not from assets" so port_rom.c
 * resolves from the ROM instead. */
int Port_AreSpritePtrsLoadedFromAssets(void) { return 0; }
int Port_IsAreaTablePtrFromAssets(u32 area, const void* ptr) { (void)area; (void)ptr; return 0; }
int Port_RefreshAreaDataFromAssets(u32 area) { (void)area; return 0; }
/* Asset-pipeline loaders/status referenced by port_rom.c (real impls in the
 * excluded port_asset_loader.cpp). No-ops on N64 — data comes from the ROM. */
int Port_LoadAreaTablesFromAssets(void) { return 0; }
int Port_LoadSpritePtrsFromAssets(void) { return 0; }
int Port_LoadTextsFromAssets(void) { return 0; }
void Port_LogAssetLoaderStatus(void) {}

/* ===== 3. Subsystem stubs (real N64 impls in later phases) ============= */

/* Save backend: TMC uses the GBA 8 KiB EEPROM layout (8-byte blocks up to
 * 0x1FA8). N64 EEPROM tops out at 2 KiB, so back the unchanged GBA EEPROM API
 * with cartridge SRAM and advertise sram256k in build.sh. */
static int sN64SaveInit;
static int sN64SaveOk;
static u32 sN64SaveBytes;

static int N64_SaveEnsure(u32 requestedBytes) {
    if (!sN64SaveInit) {
        sram_init();
        int sz = sram_detect();
        sN64SaveOk = (sz >= (int)requestedBytes);
        sN64SaveBytes = sN64SaveOk ? (u32)sz : 0;
        sN64SaveInit = 1;
        debugf("[save] sram_detect=%d bytes requested=%lu ok=%d\n", sz, (unsigned long)requestedBytes, sN64SaveOk);
    }
    return sN64SaveOk && requestedBytes <= sN64SaveBytes;
}

u16 EEPROMConfigure(u16 unk_1) {
    u32 bytes;
    if (unk_1 == 4) {
        bytes = 0x200u;
    } else if (unk_1 == 0x40) {
        bytes = 0x2000u;
    } else {
        bytes = 0x200u; /* match GBA routine: invalid selects 512-byte mode */
    }
    return N64_SaveEnsure(bytes) ? 0 : EEPROM_UNSUPPORTED_TYPE;
}

u16 EEPROMRead(u16 address, u16* data) {
    u32 offset = (u32)address * 8u;
    if (!N64_SaveEnsure(0x2000u) || offset + 8u > sN64SaveBytes)
        return EEPROM_OUT_OF_RANGE;
    data_cache_hit_writeback_invalidate(data, 8);
    int read = sram_read(data, offset, 8);
    data_cache_hit_writeback_invalidate(data, 8);
    return (read == 8) ? 0 : EEPROM_OUT_OF_RANGE;
}

u16 EEPROMCompare(u16 address, const u16* data) {
    u16 tmp[4];
    u16 ret = EEPROMRead(address, tmp);
    if (ret != 0)
        return ret;
    return (memcmp(tmp, data, 8) == 0) ? 0 : EEPROM_COMPARE_FAILED;
}

u16 EEPROMWrite0_8k_Check(u16 address, const u16* data) {
    u32 offset = (u32)address * 8u;
    if (!N64_SaveEnsure(0x2000u) || offset + 8u > sN64SaveBytes)
        return EEPROM_OUT_OF_RANGE;
    data_cache_hit_writeback_invalidate((void*)data, 8);
    return (sram_write(data, offset, 8) == 8) ? 0 : EEPROM_OUT_OF_RANGE;
}

/* Audio backend (N64 AI synth not yet implemented). These keep the m4a
 * bookkeeping ABI correct — signatures match port_m4a_backend.h exactly (the
 * include above makes that a compile-time check), so port_m4a_stubs.c no longer
 * reads a garbage return value from StartSongById (was declared void here but
 * called as bool, which could spuriously MPlayStop the player). With no synth,
 * report "nothing playing": StartSongById fails (player is immediately stopped,
 * status stays consistent and song-finished waits resolve at once → no audio
 * hang) and IsPlayerActive is always false. */
bool Port_M4A_Backend_Init(uint32_t sampleRate) { (void)sampleRate; return false; }
void Port_M4A_Backend_Shutdown(void) {}
void Port_M4A_Backend_Reset(void) {}
void Port_M4A_Backend_SoundInit(uint32_t soundMode) { (void)soundMode; }
void Port_M4A_Backend_SetSoundMode(uint32_t soundMode) { (void)soundMode; }
void Port_M4A_Backend_SetVSyncEnabled(bool enabled) { (void)enabled; }
bool Port_M4A_Backend_StartSongById(uint8_t playerIndex, uint16_t songId) { (void)playerIndex; (void)songId; return false; }
void Port_M4A_Backend_StartSong(uint8_t playerIndex, const SongHeader* songHeader) { (void)playerIndex; (void)songHeader; }
void Port_M4A_Backend_StopPlayer(uint8_t playerIndex) { (void)playerIndex; }
void Port_M4A_Backend_ContinuePlayer(uint8_t playerIndex) { (void)playerIndex; }
void Port_M4A_Backend_SetTrackVolume(uint8_t playerIndex, uint16_t trackBits, uint16_t volume) { (void)playerIndex; (void)trackBits; (void)volume; }
void Port_M4A_Backend_SetTrackPan(uint8_t playerIndex, uint16_t trackBits, int8_t pan) { (void)playerIndex; (void)trackBits; (void)pan; }
bool Port_M4A_Backend_IsPlayerActive(uint8_t playerIndex) { (void)playerIndex; return false; }
void Port_M4A_Backend_Render(int16_t* outSamples, uint32_t frameCount, bool mute) {
    (void)mute;   /* no synth yet: emit silence (stereo interleaved int16) */
    if (outSamples) memset(outSamples, 0, (size_t)frameCount * 2u * sizeof(int16_t));
}

/* Soft-slots / input (→ N64 pad in Phase 6) */
int  Port_SoftSlots_GetEffectiveBItem(void) { return 0; }
int  Port_SoftSlots_IsBHeld(void) { return 0; }
int  Port_SoftSlots_GetAssignment(u32 slot) { (void)slot; return 0; }
void Port_SoftSlots_SetAssignment(u32 slot, u32 item) { (void)slot; (void)item; }
void Port_SoftSlots_NotifyPauseActive(int active) { (void)active; }
int  Port_Config_GetLeftStick(float* outX, float* outY) { if (outX) *outX = 0; if (outY) *outY = 0; return 0; }

/* TTS (not on N64) */
void Port_TTS_Speak(const char* s) { (void)s; }
int  Port_TTS_GetEnabled(void) { return 0; }
void Port_TTS_Stop(void) {}

/* Feature toggles */
int  Port_Reborn_IsEnabled(void) { return 0; }
int  Port_Reborn_ConsumeJustResumed(void) { return 0; }
int  Port_AudioMute_ShouldSuppress(u32 category) { (void)category; return 0; }
int  Rando_OverrideItem(u32 item, u32 ctx) { (void)ctx; return (int)item; }

/* Engine fns from asm/TUs not yet compiled. File-select/dialogs are compiled
 * from src/fileselect.c now that the SRAM save backend exists. */
