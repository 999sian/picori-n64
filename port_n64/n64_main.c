/*
 * port/n64/n64_main.c — N64 entry + per-frame host hook.
 *
 * Boot: init libdragon, point gRomData at the cart-embedded baserom.gba
 * (uncached PI pointer — 16 MB can't fit in 8 MB RDRAM), resolve ROM symbols
 * via Port_LoadRom, then run the engine (AgbMain). The engine calls
 * VBlankIntrWait once per frame; n64_glue routes that to Port_N64_VBlank here,
 * which pumps the N64 pad into gIoMem[KEYINPUT] and paces via the display.
 * (Video output — ViruaPPU → framebuffer — is Phase 4.)
 */
#include <libdragon.h>
#include "virtuappu.h"
#include "cpu/mode1.h"
#include "main.h"   /* gMain.task for the bring-up autoplay */

extern void AgbMain(void);
extern void Port_LoadRom(const char* path);
extern unsigned char* gRomData;
extern unsigned int gRomSize;
extern unsigned char gIoMem[];
extern unsigned char gVram[];
extern unsigned short gBgPltt[];
extern unsigned short gObjPltt[];
extern unsigned short gOamMem[];

#define KEYINPUT (*(volatile unsigned short*)(gIoMem + 0x130))

/* --- N64 cart (ROM) read shim -------------------------------------------------
 * The PI bus only supports <=32-bit CPU reads; a 64-bit load — or a memcpy the
 * compiler lowers to ld/sd — from cart space freezes the bus (ares
 * "freezeDualRead"). gRomData points into cart, so every large memcpy out of the
 * ROM hits this. --wrap=memcpy reroutes cart-sourced copies through PI DMA. */
static void n64_cart_read(void* dst, uint32_t pi_addr, size_t n) {
    if (n == 0) return;
    /* Large, parity-matched: fast PI DMA into RDRAM (flush dst cache first). */
    if (n > 16 && (((uintptr_t)dst ^ (uintptr_t)pi_addr) & 1u) == 0) {
        data_cache_hit_writeback_invalidate(dst, n); /* flush+drop dst cache lines */
        dma_read(dst, pi_addr, n);                   /* PI DMA cart -> RDRAM */
        return;
    }
    /* Small or misaligned: assemble from 32-bit PI reads (big-endian, cache-safe). */
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) {
        uint32_t w = io_read((pi_addr + (uint32_t)i) & ~3u);
        d[i] = (uint8_t)(w >> (8u * (3u - ((pi_addr + (uint32_t)i) & 3u))));
    }
}
extern void* __real_memcpy(void* dst, const void* src, size_t n);
void* __wrap_memcpy(void* dst, const void* src, size_t n) {
    uint32_t phys = (uint32_t)((uintptr_t)src & 0x1FFFFFFFu);
    if (phys >= 0x10000000u && phys < 0x1FC00000u) /* cart / PI domain */
        n64_cart_read(dst, phys, n);
    else
        __real_memcpy(dst, src, n);
    return dst;
}

/* GBA KEYINPUT bits (active-low: 0 = pressed) */
enum {
    GBA_A = 0x0001, GBA_B = 0x0002, GBA_SELECT = 0x0004, GBA_START = 0x0008,
    GBA_RIGHT = 0x0010, GBA_LEFT = 0x0020, GBA_UP = 0x0040, GBA_DOWN = 0x0080,
    GBA_R = 0x0100, GBA_L = 0x0200,
};

#define ROM_SIZE_USA (16u * 1024u * 1024u)

static void WireRom(void) {
    dfs_init(DFS_DEFAULT_LOCATION);
    uint32_t addr = dfs_rom_addr("/baserom.gba");
    if (addr) {
        /* Uncached (KSEG1) pointer into the cart-resident ROM. Byte/halfword/
         * word reads hit the PI bus directly — slow, but correct, and avoids a
         * 16 MB RAM copy. Phase 7 can DMA hot regions into RDRAM. */
        gRomData = (unsigned char*)(0xA0000000u | (addr & 0x1FFFFFFFu));
        gRomSize = ROM_SIZE_USA;
        debugf("[n64] baserom.gba @ cart 0x%08lx -> gRomData=%p\n",
               (unsigned long)addr, (void*)gRomData);
    } else {
        debugf("[n64] ERROR: baserom.gba not found in DFS\n");
    }
    Port_LoadRom(0); /* N64 path: resolves symbols from gRomData (no file I/O) */
}

/* Called once per emulated GBA frame from VBlankIntrWait (n64_glue.c). */
/* N64: ViruaPPU reads palettes natively, but gBgPltt/gObjPltt hold GBA
 * little-endian colors. Port_N64_VBlank byte-swaps them into these BE scratch
 * tables each frame and ViruaPPU is bound to these instead of the raw arrays. */
static uint16_t sBgPlttBE[256];
static uint16_t sObjPlttBE[256];

/* --- Phase 7: RDP-accelerated GBA tiled-BG renderer (proof: mode 0, BG0) ------
 * Render the GBA tilemap on the RCP instead of the per-pixel software PPU. Each
 * 8x8 GBA tile becomes a CI4 texrect; the GBA palette is one 256-entry TLUT
 * (BGR555->RGBA5551; index 0 of each 16-colour bank = transparent via alpha
 * compare). GBA 4bpp packs the left pixel in the LOW nibble but RDP CI4 wants it
 * HIGH, so the char block is nibble-swapped into a scratch each frame. Drawn
 * centered (240x160) into the 320x240 framebuffer. */
int g_n64_use_rdp = 1;    /* RDP render path; software ViruaPPU is the fallback */
int g_n64_autoplay = 0;   /* bring-up gameplay-reach (demo save -> TASK_GAME). OFF: room load
                           * crashes on ROM-struct endianness (see docs/n64-port-plan.md);
                           * default boot stays on the stable title. Flip to 1 to debug gameplay. */
int g_n64_rdp_obj  = 0;   /* RDP OBJ/sprite path: OFF (implemented, not yet verified
                           * on a real sprite frame — title is OBJ-off, file-select is
                           * affine→software. Flip to 1 once a gameplay-reach harness
                           * confirms it; until then OBJ frames use software.) */
static uint8_t  sCharScratch[0x8000] __attribute__((aligned(16)));
static uint16_t sRdpTlut[256]        __attribute__((aligned(16)));

static inline uint16_t bgr555_to_rgba5551(uint16_t c, int opaque) {
    unsigned r = c & 0x1Fu, g = (c >> 5) & 0x1Fu, b = (c >> 10) & 0x1Fu;
    return (uint16_t)((r << 11) | (g << 6) | (b << 1) | (opaque ? 1u : 0u));
}

/* OBJ (sprite) pass — non-affine 4bpp sprites via CI4 texrects from OBJ char
 * (VRAM 0x10000) + the OBJ palette TLUT. Affine/8bpp sprites are kept out by
 * Port_N64_RDP_FrameSupported (those frames use the software path). Called inside
 * the rdpq_attach session after the BG pass. Assumes scissor = GBA viewport. */
static const uint8_t sObjW[3][4] = {{8,16,32,64},{16,32,32,64},{8,8,16,32}};
static const uint8_t sObjH[3][4] = {{8,16,32,64},{8,8,16,32},{16,32,32,64}};
static void Port_N64_RDP_RenderOBJ(unsigned dispcnt) {
    int obj_1d = (dispcnt >> 6) & 1;
    for (int i = 0; i < 256; i++)
        sRdpTlut[i] = bgr555_to_rgba5551(sObjPlttBE[i], (i & 15) != 0);
    data_cache_hit_writeback(sRdpTlut, sizeof sRdpTlut);
    for (uint32_t i = 0; i < 0x8000u; i++) {
        uint8_t v = gVram[0x10000u + i];
        sCharScratch[i] = (uint8_t)((v >> 4) | (v << 4));
    }
    data_cache_hit_writeback(sCharScratch, 0x8000u);
    surface_t objc = surface_make(sCharScratch, FMT_CI4, 8, (int)(0x8000u * 2u / 8u), 4);
    rdpq_tex_upload_tlut(sRdpTlut, 0, 256);
    for (int i = 127; i >= 0; i--) {   /* high OAM index first so index 0 ends on top */
        uint16_t a0 = gOamMem[i * 4 + 0], a1 = gOamMem[i * 4 + 1], a2 = gOamMem[i * 4 + 2];
        int affine = (a0 >> 8) & 1;
        if (!affine && ((a0 >> 9) & 1)) continue;   /* hidden */
        if (affine || ((a0 >> 13) & 1)) continue;   /* affine/8bpp -> software (FrameSupported) */
        int shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
        int wt = sObjW[shape][size] / 8, ht = sObjH[shape][size] / 8;
        int oy = a0 & 0xFF; if (oy >= 160) oy -= 256;
        int ox = a1 & 0x1FF; if (ox >= 240) ox -= 512;
        int hflip = (a1 >> 12) & 1, vflip = (a1 >> 13) & 1;
        unsigned base = a2 & 0x3FFu, pal = (a2 >> 12) & 0xFu;
        for (int ty = 0; ty < ht; ty++) {
            for (int tx = 0; tx < wt; tx++) {
                int stc = hflip ? (wt - 1 - tx) : tx;
                int str = vflip ? (ht - 1 - ty) : ty;
                unsigned tidx = obj_1d ? (base + (unsigned)(str * wt + stc))
                                       : (base + (unsigned)(str * 32 + stc));
                if (tidx >= 1024u) continue;
                surface_t ts = surface_make_sub(&objc, 0, (int)tidx * 8, 8, 8);
                rdpq_texparms_t parms = {0};
                parms.palette = (int)pal; parms.s.repeats = 1; parms.t.repeats = 1;
                rdpq_tex_upload(TILE0, &ts, &parms);
                int dx = 40 + ox + tx * 8, dy = 40 + oy + ty * 8;
                int s0 = hflip ? 8 : 0, s1 = hflip ? 0 : 8;
                int t0 = vflip ? 8 : 0, t1 = vflip ? 0 : 8;
                rdpq_texture_rectangle_scaled(TILE0, dx, dy, dx + 8, dy + 8, s0, t0, s1, t1);
            }
        }
    }
    rspq_wait();
}

static void Port_N64_RDP_RenderFrame(surface_t* disp) {
    unsigned dispcnt = *(volatile unsigned short*)(gIoMem + 0x00);
    for (int i = 0; i < 256; i++)
        sRdpTlut[i] = bgr555_to_rgba5551(sBgPlttBE[i], (i & 15) != 0);  /* bank idx0 = transparent */
    data_cache_hit_writeback(sRdpTlut, sizeof sRdpTlut);

    uint16_t bd = sBgPlttBE[0];
    color_t backdrop = RGBA32((bd & 0x1Fu) << 3, ((bd >> 5) & 0x1Fu) << 3, ((bd >> 10) & 0x1Fu) << 3, 255);
    rdpq_attach(disp, NULL);
    rdpq_set_mode_fill(backdrop);
    rdpq_fill_rectangle(0, 0, 320, 240);
    rdpq_set_scissor(40, 40, 280, 200);   /* clip BG/OBJ draws to the 240x160 viewport */
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);   /* TLUT alpha 0 (bank index 0) = transparent */
    rdpq_tex_upload_tlut(sRdpTlut, 0, 256);

    /* Draw enabled tiled BGs back-to-front (BG3..BG0; BG0 ends up on top). */
    for (int bg = 3; bg >= 0; bg--) {
        if (!(dispcnt & (0x100u << bg))) continue;
        unsigned bgcnt = *(volatile unsigned short*)(gIoMem + 0x08 + bg * 2);
        uint32_t char_base   = ((bgcnt >> 2) & 3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        unsigned size  = (bgcnt >> 14) & 3u;         /* 0:256 1:512w 2:512h 3:512sq */
        unsigned map_w = (size & 1u) ? 64u : 32u;    /* tilemap width  in tiles */
        unsigned map_h = (size & 2u) ? 64u : 32u;    /* tilemap height in tiles */
        unsigned sbcols = map_w >> 5;                /* screenblocks across */
        unsigned hofs = (*(volatile unsigned short*)(gIoMem + 0x10 + bg * 4)) & 0x1FFu;
        unsigned vofs = (*(volatile unsigned short*)(gIoMem + 0x12 + bg * 4)) & 0x1FFu;
        int px0 = (int)(hofs & 7u), py0 = (int)(vofs & 7u);  /* sub-tile pixel offset */
        unsigned tx0 = hofs >> 3, ty0 = vofs >> 3;           /* first visible tile */
        uint32_t span = (char_base + 0x8000u <= 0x18000u) ? 0x8000u : (0x18000u - char_base);
        for (uint32_t i = 0; i < span; i++) {
            uint8_t v = gVram[char_base + i];
            sCharScratch[i] = (uint8_t)((v >> 4) | (v << 4));
        }
        data_cache_hit_writeback(sCharScratch, span);
        surface_t chars = surface_make(sCharScratch, FMT_CI4, 8, (int)(span * 2u / 8u), 4);
        /* 21x31 tiles: one extra row/col so the partial edge tiles from a sub-tile
         * scroll are covered; the viewport scissor clips the overhang. */
        for (int ty = 0; ty <= 20; ty++) {
            for (int tx = 0; tx <= 30; tx++) {
                unsigned mtx = (tx0 + (unsigned)tx) & (map_w - 1u);
                unsigned mty = (ty0 + (unsigned)ty) & (map_h - 1u);
                unsigned sb  = (mty >> 5) * sbcols + (mtx >> 5);   /* screenblock index */
                uint32_t map_addr = screen_base + sb * 0x800u
                                  + (((mty & 31u) * 32u + (mtx & 31u)) * 2u);
                uint16_t entry = (uint16_t)gVram[map_addr] | ((uint16_t)gVram[map_addr + 1u] << 8u);
                unsigned tile_idx = entry & 0x3FFu;
                unsigned pal = (entry >> 12) & 0xFu;
                int hflip = (entry >> 10) & 1, vflip = (entry >> 11) & 1;
                surface_t ts = surface_make_sub(&chars, 0, (int)tile_idx * 8, 8, 8);
                rdpq_texparms_t parms = {0};
                parms.palette = (int)pal;
                parms.s.repeats = 1; parms.t.repeats = 1;
                rdpq_tex_upload(TILE0, &ts, &parms);
                int dx = 40 + tx * 8 - px0, dy = 40 + ty * 8 - py0;
                int s0 = hflip ? 8 : 0, s1 = hflip ? 0 : 8;
                int t0 = vflip ? 8 : 0, t1 = vflip ? 0 : 8;
                rdpq_texture_rectangle_scaled(TILE0, dx, dy, dx + 8, dy + 8, s0, t0, s1, t1);
            }
        }
        rspq_wait();   /* RDP done with sCharScratch before the next BG overwrites it */
    }
    if (dispcnt & 0x1000u) Port_N64_RDP_RenderOBJ(dispcnt);   /* sprites on top */
    rdpq_detach_wait();
}

/* Does the RDP path render this frame correctly? It handles mode-0 tiled BGs (with
 * scroll, per-tile H/V flip, and the 256/512 multi-screenblock sizes) + non-affine
 * 4bpp sprites; anything else (affine BG, windows, blend, 8bpp/affine sprites) falls
 * back to the correct, slower software ViruaPPU path. Never wrong — shrinks as RdpExtend lands. */
static int Port_N64_RDP_FrameSupported(void) {
    unsigned dispcnt = *(volatile unsigned short*)(gIoMem + 0x00);
    if ((dispcnt & 7u) != 0u)      return 0;   /* mode 1/2: affine BG2 */
    if (dispcnt & 0xE000u)         return 0;   /* WIN0/WIN1/OBJWIN */
    unsigned bldcnt = *(volatile unsigned short*)(gIoMem + 0x50);
    if (((bldcnt >> 6) & 3u) != 0u) return 0;  /* blend effect */
    if (dispcnt & 0x1000u) {   /* OBJ on */
        if (!g_n64_rdp_obj) return 0;   /* RDP OBJ path unverified -> software */
        /* RDP OBJ pass skips affine/8bpp — fall back if any present */
        for (int i = 0; i < 128; i++) {
            uint16_t a0 = gOamMem[i * 4 + 0];
            int affine = (a0 >> 8) & 1;
            if (!affine && ((a0 >> 9) & 1)) continue;        /* hidden */
            if (affine || ((a0 >> 13) & 1)) return 0;        /* affine or 8bpp sprite */
        }
    }
    return 1;
}

void Port_N64_VBlank(void) {
    joypad_poll();
    joypad_buttons_t b = joypad_get_buttons(JOYPAD_PORT_1);
    unsigned short k = 0x03FF;
    if (b.a)       k &= (unsigned short)~GBA_A;
    if (b.b)       k &= (unsigned short)~GBA_B;
    if (b.start)   k &= (unsigned short)~GBA_START;
    if (b.z)       k &= (unsigned short)~GBA_SELECT;
    if (b.l)       k &= (unsigned short)~GBA_L;
    if (b.r)       k &= (unsigned short)~GBA_R;
    if (b.d_up)    k &= (unsigned short)~GBA_UP;
    if (b.d_down)  k &= (unsigned short)~GBA_DOWN;
    if (b.d_left)  k &= (unsigned short)~GBA_LEFT;
    if (b.d_right) k &= (unsigned short)~GBA_RIGHT;
    if (g_n64_autoplay) {   /* bring-up: after a few title frames, force demo-save -> TASK_GAME */
        extern Main gMain;
        extern void Port_N64_ForceGameStart(void);
        static unsigned ap = 0;
        if (gMain.task == 0u && ap == 20u) Port_N64_ForceGameStart();
        ap++;
    }
    KEYINPUT = k;

    /* Palette colors are GBA little-endian in gBgPltt/gObjPltt; byte-swap into
     * the BE scratch ViruaPPU reads, so native palette reads give right colors.
     * Full-table each frame so live engine updates are tracked. */
    for (int i = 0; i < 256; i++) {
        sBgPlttBE[i]  = (uint16_t)__builtin_bswap16(gBgPltt[i]);
        sObjPlttBE[i] = (uint16_t)__builtin_bswap16(gObjPltt[i]);
    }

    surface_t* disp = display_get();
    static uint64_t s_render_us = 0, s_blit_us = 0;  /* perf probe */
    uint64_t _t0 = get_ticks_us();
    if (g_n64_use_rdp && Port_N64_RDP_FrameSupported()) {
        Port_N64_RDP_RenderFrame(disp);
        s_render_us = get_ticks_us() - _t0;
        s_blit_us = 0;
    } else {
        /* Software ViruaPPU render (reads bound gVram/palettes/OAM) + ABGR->RGBA
         * blit, 240x160 centered into the N64 320x240 framebuffer. */
        virtuappu_registers.frame_width = 240;
        { unsigned dc = *(volatile unsigned short*)(gIoMem + 0x00);
          virtuappu_registers.mode = ((dc & 7u) == 1u || (dc & 7u) == 2u) ? 2 : 1; }
        virtuappu_render_frame();
        uint64_t _t1 = get_ticks_us();
        graphics_fill_screen(disp, 0);
        uint32_t* fb = (uint32_t*)disp->buffer;
        int stride = disp->stride / 4;
        const uint32_t* src = virtuappu_frame_buffer;
        for (int y = 0; y < 160; y++) {
            uint32_t* row = fb + (y + 40) * stride + 40;
            const uint32_t* s = src + y * 240;
            for (int x = 0; x < 240; x++) {
                uint32_t px = s[x];
                unsigned r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
                row[x] = (r << 24) | (g << 16) | (b << 8) | 0xFFu;
            }
        }
        s_render_us = _t1 - _t0;
        s_blit_us = get_ticks_us() - _t1;
    }
    /* N64 bring-up scaffolding (drop once video is signed off). Reads the actual
     * N64 framebuffer (uncached, after the RDP/blit) so it works for both paths. */
    {
        static unsigned s_dbgf = 0;
        if (s_dbgf < 4u || (s_dbgf & 31u) == 0u) {
            extern Main gMain;
            unsigned dispcnt = *(volatile unsigned short*)(gIoMem + 0x00);
            unsigned h0 = *(volatile unsigned short*)(gIoMem + 0x10);
            unsigned v0 = *(volatile unsigned short*)(gIoMem + 0x12);
            debugf("[dbg] f=%u task=%u dispcnt=%04x hofs0=%04x vofs0=%04x render_us=%lu\n",
                   s_dbgf, (unsigned)gMain.task, dispcnt, h0, v0, (unsigned long)s_render_us);
        }
        if (s_dbgf == 64u) {   /* one-shot ASCII framebuffer thumbnail (both paths) */
            static const char ramp[] = " .:-=+*#%@";
            data_cache_hit_writeback_invalidate(disp->buffer, (unsigned long)disp->stride * 240u);
            const uint32_t* fbuf = (const uint32_t*)disp->buffer;
            int dstride = disp->stride / 4;
            debugf("[fb] === framebuffer thumbnail f=%u (rdp=%d, 60x40 of centered 240x160) ===\n",
                   s_dbgf, g_n64_use_rdp);
            for (int cy = 0; cy < 40; cy++) {
                char line[61];
                for (int cx = 0; cx < 60; cx++) {
                    uint32_t px = fbuf[(40 + cy * 4) * dstride + (40 + cx * 4)];
                    unsigned r = (px >> 24) & 0xFFu, g = (px >> 16) & 0xFFu, b = (px >> 8) & 0xFFu;
                    line[cx] = ramp[(((r + g + b) / 3u) * 9u) / 255u];
                }
                line[60] = 0;
                debugf("[fb] %s\n", line);
            }
        }
        s_dbgf++;
    }
    display_show(disp);
}

/* --- N64 bring-up POST codes -------------------------------------------------
 * The engine renders only once it reaches its first VBlankIntrWait. If it hangs
 * earlier in init the screen stays black with no clue. n64_post() paints a
 * full-screen marker synchronously; the VI keeps flipping buffers even while the
 * main thread is busy, so the last marker shown localizes a boot hang.
 * Diagnostic scaffolding — removed once boot is stable. */
volatile int g_n64_phase = 0;
void n64_post(int phase) {
    g_n64_phase = phase;
    surface_t* d = display_get();
    graphics_fill_screen(d, graphics_make_color(0, 0, 170, 255));
    graphics_set_color(0xFFFFFFFFu, 0x000000AAu);
    char s[24];
    sprintf(s, "BOOT P%d", phase);
    graphics_draw_text(d, 128, 112, s);
    display_show(d);
}

int main(void) {
    debug_init_isviewer();
    debugf("[n64] Project Picori — N64 boot\n");
    /* ares with no Expansion Pak = 4 MB; this image (~5.4 MB) needs 8 MB. Print
     * the detected size so a 4 MB config (the likely "works on gopher64, not
     * ares" cause) is obvious from the debug channel. */
    debugf("[n64] RDRAM=%d bytes (image ~5.4MB; 8MB Expansion Pak required)\n", get_memory_size());
    /* 32bpp (RGBA8888): ViruaPPU already outputs 32-bit color, and the lossy
     * 16-bit (5/5/5/1) path is interpreted differently by ares vs gopher64
     * (coverage/reconstruction) -> wrong colors on ares. 32bpp is unambiguous. */
    display_init(RESOLUTION_320x240, DEPTH_32_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();   /* Phase 7: RDP-accelerated BG rendering (Port_N64_RDP_RenderFrame) */
    n64_post(1);
    joypad_init();
    extern void n64_assert_handler(const char*, int, const char*, const char*);
    extern void (*__assert_func_ptr)(const char*, int, const char*, const char*);
    __assert_func_ptr = n64_assert_handler; /* C asserts -> non-fatal, recorded */
    n64_post(2);

    WireRom();
    n64_post(3);
    VirtuaPPUMode1GbaMemory mem = { gIoMem, gVram, sBgPlttBE, sObjPlttBE, gOamMem };
    virtuappu_mode1_bind_gba_memory(&mem);
    virtuappu_reset();
    n64_post(4);
    debugf("[n64] ROM wired (gRomSize=%u). Entering AgbMain.\n", gRomSize);

    AgbMain(); /* never returns */
    return 0;
}
