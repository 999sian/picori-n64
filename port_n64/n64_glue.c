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
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>

#ifdef TMC_N64
/* libdragon PI DMA + cache ops (declared, not via <libdragon.h>, to avoid type
 * clashes with the game headers above). */
extern void dma_read(void* ram_address, unsigned long pi_address, unsigned long len);
extern void data_cache_hit_writeback_invalidate(volatile void* addr, unsigned long length);
static u8 sLz77DmaBuf[0x18000] __attribute__((aligned(16))); /* 96KB cart->RDRAM stage */
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
            u32 dsz  = (u32)s[1] | ((u32)s[2] << 8) | ((u32)s[3] << 16); /* decompressed size */
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
    u32 size = (u32)s[1] | ((u32)s[2] << 8) | ((u32)s[3] << 16);
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
/* dest is a raw GBA address (e.g. VRAM 0x06000000) and src is usually a native
 * cart pointer (&gGlobalGfxAndPalettes[..]) but may be a GBA address too.
 * Resolve both to native pointers before decompressing, exactly as the PC
 * port_bios.c versions do — without this, compressed gfx/tilemaps decompress to
 * the literal GBA address and never reach gVram (black screen, #N64). */
void LZ77UnCompWram(const void* src, void* dest) {
    Lz77Decompress(port_resolve_addr((uintptr_t)src), port_resolve_addr((uintptr_t)dest));
}
void LZ77UnCompVram(const void* src, void* dest) {
    Lz77Decompress(port_resolve_addr((uintptr_t)src), port_resolve_addr((uintptr_t)dest));
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
void BgAffineSet(struct BgAffineSrcData* src, struct BgAffineDstData* dest, s32 count) { (void)src; (void)dest; (void)count; }
void ObjAffineSet(struct ObjAffineSrcData* src, void* dest, s32 count, s32 offset) { (void)src; (void)dest; (void)count; (void)offset; }

/* ===== 2. ROM / asset data (filled by the Phase-3 boot loader) ========= */

/* Opaque blob; defined here only to satisfy the extern (figurine menu data). */
const unsigned char gUnk_080FC3E4[2048] = {0};

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
int  Port_LoadPaletteGroupFromAssets(u32 a) { (void)a; return 0; }
int  Port_LoadGfxGroupFromAssets(u32 a) { (void)a; return 0; }
void Port_LogTextLookup(u32 a) { (void)a; }
void Port_DumpAssetEnvironment(void) {}
/* Asset-pipeline predicates referenced by port_rom.c (real impls in the
 * excluded port_asset_loader.cpp). Return "not from assets" so port_rom.c
 * resolves from the ROM instead. */
int  Port_AreSpritePtrsLoadedFromAssets(void) { return 0; }
int  Port_IsAreaTablePtrFromAssets(u32 addr) { (void)addr; return 0; }
void Port_RefreshAreaDataFromAssets(void) {}
/* Asset-pipeline loaders/status referenced by port_rom.c (real impls in the
 * excluded port_asset_loader.cpp). No-ops on N64 — data comes from the ROM. */
void Port_LoadAreaTablesFromAssets(void) {}
void Port_LoadSpritePtrsFromAssets(void) {}
void Port_LoadTextsFromAssets(void) {}
void Port_LogAssetLoaderStatus(void) {}

/* ===== 3. Subsystem stubs (real N64 impls in later phases) ============= */

/* EEPROM / save */
u16 EEPROMConfigure(u16 unk_1) { (void)unk_1; return 0; }
u16 EEPROMRead(u16 address, u16* data) { (void)address; (void)data; return 0; }
u16 EEPROMCompare(u16 address, const u16* data) { (void)address; (void)data; return 0; }
u16 EEPROMWrite0_8k_Check(u16 address, const u16* data) { (void)address; (void)data; return 0; }

/* Audio backend (→ N64 AI in Phase 5) */
void Port_M4A_Backend_SoundInit(void) {}
void Port_M4A_Backend_StartSongById(u32 id) { (void)id; }
void Port_M4A_Backend_StopPlayer(void) {}
void Port_M4A_Backend_ContinuePlayer(void) {}
int  Port_M4A_Backend_IsPlayerActive(void) { return 0; }
void Port_M4A_Backend_SetVSyncEnabled(int e) { (void)e; }
void Port_M4A_Backend_SetTrackVolume(u32 t, u32 v) { (void)t; (void)v; }

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

/* Engine fns from the two TUs not yet compiled (fileselect.c, asm) */
void  FileSelectTask(void) {}
void* CreateDialogBox(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return 0; }
void  sub_08050384(void) {}
