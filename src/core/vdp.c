/* vdp.c - Mega Drive VDP: ports, DMA, interrupts, scanline renderer */
#include <string.h>
#include "core_internal.h"
#include "m68k.h"

static uint8_t  vram[0x10000];
static uint16_t cram[64];
static uint16_t vsram[40];
static uint8_t  reg[24];

/* control port state */
static bool     ctrl_pending;      /* waiting for second control word */
static uint8_t  code;              /* CD5..CD0 */
static uint32_t addr;              /* 16-bit VDP address */
static bool     dma_fill_pending;

/* interrupt state */
static bool vint_pending, hint_pending;
static bool status_vint_flag;      /* status bit 7 (F) */
static int  hint_counter;

/* cached per-frame */
static uint32_t cram_rgba[64];

#define REG_MODE1      reg[0]
#define REG_MODE2      reg[1]
#define REG_PLANE_A    reg[2]
#define REG_WINDOW     reg[3]
#define REG_PLANE_B    reg[4]
#define REG_SPRITE     reg[5]
#define REG_BACKDROP   reg[7]
#define REG_HINT       reg[10]
#define REG_MODE3      reg[11]
#define REG_MODE4      reg[12]
#define REG_HSCROLL    reg[13]
#define REG_AUTOINC    reg[15]
#define REG_PLANE_SIZE reg[16]
#define REG_WIN_X      reg[17]
#define REG_WIN_Y      reg[18]
#define REG_DMA_LEN_L  reg[19]
#define REG_DMA_LEN_H  reg[20]
#define REG_DMA_SRC_L  reg[21]
#define REG_DMA_SRC_M  reg[22]
#define REG_DMA_SRC_H  reg[23]

#define DISPLAY_ON     (REG_MODE2 & 0x40)
#define VINT_ENABLED   (REG_MODE2 & 0x20)
#define DMA_ENABLED    (REG_MODE2 & 0x10)
#define HINT_ENABLED   (REG_MODE1 & 0x10)
#define H40            (REG_MODE4 & 0x01)

/* 3-bit DAC level to 8-bit (linear approximation) */
static const uint8_t lvl3[8] = { 0, 36, 73, 109, 146, 182, 219, 255 };

static uint32_t cram_to_rgba(uint16_t c)
{
    uint8_t r = lvl3[(c >> 1) & 7];
    uint8_t g = lvl3[(c >> 5) & 7];
    uint8_t b = lvl3[(c >> 9) & 7];
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u;
}

void vdp_reset(void)
{
    memset(vram, 0, sizeof vram);
    memset(cram, 0, sizeof cram);
    memset(vsram, 0, sizeof vsram);
    memset(reg, 0, sizeof reg);
    memset(cram_rgba, 0, sizeof cram_rgba);
    ctrl_pending = dma_fill_pending = false;
    code = 0;
    addr = 0;
    vint_pending = hint_pending = false;
    status_vint_flag = false;
    hint_counter = 0;
}

/* ------------------------------------------------------------ interrupts */
static void update_irq(void)
{
    if (vint_pending && VINT_ENABLED)      m68k_set_irq(6);
    else if (hint_pending && HINT_ENABLED) m68k_set_irq(4);
    else                                   m68k_set_irq(0);
}

void vdp_int_ack(int level)
{
    if (level == 6) { vint_pending = false; status_vint_flag = false; }
    if (level == 4) hint_pending = false;
    update_irq();
}

void vdp_line_start(int line)
{
    if (line == 0)
        hint_counter = REG_HINT;

    if (line < MD_LINES_ACTIVE) {
        if (--hint_counter < 0) {
            hint_counter = REG_HINT;
            hint_pending = true;
        }
    } else {
        hint_counter = REG_HINT;
    }

    if (line == MD_LINES_ACTIVE) {   /* start of vblank */
        vint_pending = true;
        status_vint_flag = true;
    }
    update_irq();
}

/* ------------------------------------------------------------------- DMA */
static void data_port_write_word(uint16_t v);

static void dma_68k_transfer(void)
{
    uint32_t len = REG_DMA_LEN_L | (REG_DMA_LEN_H << 8);
    if (len == 0) len = 0x10000;
    uint32_t src = ((REG_DMA_SRC_L | (REG_DMA_SRC_M << 8) |
                    ((REG_DMA_SRC_H & 0x7F) << 16)) << 1) & 0xFFFFFF;
    while (len--) {
        data_port_write_word((uint16_t)m68k_read_memory_16(src));
        src += 2;
    }
}

static void dma_vram_fill(uint16_t v)
{
    uint32_t len = REG_DMA_LEN_L | (REG_DMA_LEN_H << 8);
    if (len == 0) len = 0x10000;
    uint8_t fill = v >> 8;
    while (len--) {
        vram[(addr ^ 1) & 0xFFFF] = fill;
        addr = (addr + REG_AUTOINC) & 0x1FFFF;
    }
}

static void dma_vram_copy(void)
{
    uint32_t len = REG_DMA_LEN_L | (REG_DMA_LEN_H << 8);
    if (len == 0) len = 0x10000;
    uint32_t src = REG_DMA_SRC_L | (REG_DMA_SRC_M << 8);
    while (len--) {
        vram[addr & 0xFFFF] = vram[src & 0xFFFF];
        src++;
        addr = (addr + REG_AUTOINC) & 0x1FFFF;
    }
}

/* ------------------------------------------------------------- I/O ports */
static void data_port_write_word(uint16_t v)
{
    switch (code & 0xF) {
    case 1: {                       /* VRAM write (byte-swapped when odd) */
        uint32_t a = addr & 0xFFFF;
        if (a & 1) {
            vram[a & 0xFFFE] = v & 0xFF;
            vram[a | 1]      = v >> 8;
        } else {
            vram[a]     = v >> 8;
            vram[a + 1] = v & 0xFF;
        }
        break;
    }
    case 3: {                       /* CRAM write */
        int i = (addr >> 1) & 63;
        cram[i] = v;
        cram_rgba[i] = cram_to_rgba(v);
        break;
    }
    case 5:                         /* VSRAM write */
        if (((addr >> 1) % 64) < 40)
            vsram[(addr >> 1) % 64] = v & 0x7FF;
        break;
    default:
        break;
    }
    addr = (addr + REG_AUTOINC) & 0x1FFFF;
}

void vdp_write_data(uint16_t v)
{
    ctrl_pending = false;
    if (dma_fill_pending) {
        dma_fill_pending = false;
        dma_vram_fill(v);
        return;
    }
    data_port_write_word(v);
}

uint16_t vdp_read_data(void)
{
    ctrl_pending = false;
    uint16_t v = 0;
    switch (code & 0xF) {
    case 0: {                       /* VRAM read */
        uint32_t a = addr & 0xFFFE;
        v = (vram[a] << 8) | vram[a + 1];
        break;
    }
    case 8:                         /* CRAM read */
        v = cram[(addr >> 1) & 63];
        break;
    case 4:                         /* VSRAM read */
        v = vsram[(addr >> 1) % 64 < 40 ? (addr >> 1) % 64 : 0];
        break;
    default:
        break;
    }
    addr = (addr + REG_AUTOINC) & 0x1FFFF;
    return v;
}

void vdp_write_control(uint16_t v)
{
    if (!ctrl_pending) {
        if ((v & 0xE000) == 0x8000) {           /* register write */
            uint8_t r = (v >> 8) & 0x1F;
            if (r < 24) reg[r] = v & 0xFF;
            code = 0;
            return;
        }
        code = (code & 0x3C) | ((v >> 14) & 3);
        addr = (addr & 0x1C000) | (v & 0x3FFF);
        ctrl_pending = true;
        return;
    }

    ctrl_pending = false;
    code = (uint8_t)((code & 3) | ((v >> 2) & 0x3C));
    addr = (addr & 0x3FFF) | ((uint32_t)(v & 3) << 14);

    if ((code & 0x20) && DMA_ENABLED) {         /* CD5: DMA operation */
        uint8_t mode = REG_DMA_SRC_H >> 6;
        if (mode < 2)       dma_68k_transfer();
        else if (mode == 2) dma_fill_pending = true;
        else                dma_vram_copy();
    }
}

uint16_t vdp_read_status(void)
{
    ctrl_pending = false;
    uint16_t s = 0x3400 | 0x0200;                 /* FIFO empty, open bus */
    bool vblank = md_line >= MD_LINES_ACTIVE || !DISPLAY_ON;
    if (vblank) s |= 0x0008;
    if (status_vint_flag) s |= 0x0080;
    /* rough hblank: last ~25% of the line (from 68k progress in timeslice) */
    if (m68k_cycles_run() > (M68K_CYCLES_LINE * 3) / 4) s |= 0x0004;
    return s;
}

uint16_t vdp_read_hv(void)
{
    int v = md_line;
    if (v > 0xEA) v -= 6;             /* NTSC V counter jump */
    /* H from how far into the line the 68k is (very approximate) */
    int h = (m68k_cycles_run() * (H40 ? 0xB6 : 0x93)) / M68K_CYCLES_LINE;
    return (uint16_t)(((v & 0xFF) << 8) | (h & 0xFF));
}

/* --------------------------------------------------------------- render
 * Per pixel we track a "rank" so layers composite by priority:
 *   0 backdrop, 1 B-low, 2 A-low, 3 sprite-low, 4 B-high, 5 A-high, 6 sprite-high
 */
static uint8_t line_idx[MD_SCREEN_W];   /* CRAM index 0..63 */
static uint8_t line_rank[MD_SCREEN_W];

static int plane_dim(int bits)          /* cells */
{
    switch (bits & 3) {
    case 0: return 32;
    case 1: return 64;
    case 3: return 128;
    default: return 32;
    }
}

static void render_plane(int line, bool plane_b, int width)
{
    int pw = plane_dim(REG_PLANE_SIZE);
    int ph = plane_dim(REG_PLANE_SIZE >> 4);
    uint32_t nt = plane_b ? ((REG_PLANE_B & 0x07) << 13)
                          : ((REG_PLANE_A & 0x38) << 10);

    /* window region: carve plane A (window itself drawn separately) */
    int win_x0 = 0, win_x1 = 0;      /* [x0,x1) columns covered by window */
    if (!plane_b) {
        int wx = (REG_WIN_X & 0x1F) * 16;
        int wy = (REG_WIN_Y & 0x1F) * 8;
        bool win_line = (REG_WIN_Y & 0x80) ? (line >= wy) : (line < wy);
        if (win_line) { win_x0 = 0; win_x1 = width; }
        else if (REG_WIN_X & 0x80) { win_x0 = wx; win_x1 = width; }
        else { win_x0 = 0; win_x1 = wx; }
    }

    /* h-scroll */
    uint32_t hst = (REG_HSCROLL & 0x3F) << 10;
    uint32_t hs_entry;
    switch (REG_MODE3 & 3) {
    case 3:  hs_entry = hst + line * 4; break;          /* per line  */
    case 2:  hs_entry = hst + (line & ~7) * 4; break;   /* per cell  */
    default: hs_entry = hst; break;                     /* full      */
    }
    int hscroll = ((vram[(hs_entry + (plane_b ? 2 : 0)) & 0xFFFF] << 8) |
                    vram[(hs_entry + (plane_b ? 3 : 1)) & 0xFFFF]) & 0x3FF;

    bool vs_2cell = REG_MODE3 & 4;
    int vs_full = vsram[plane_b ? 1 : 0];

    for (int x = 0; x < width; x++) {
        if (!plane_b && x >= win_x0 && x < win_x1)
            continue;
        int vscroll = vs_2cell ? vsram[((x >> 4) * 2 + (plane_b ? 1 : 0)) % 40]
                               : vs_full;
        int px = (x - hscroll) & (pw * 8 - 1);
        int py = (line + vscroll) & (ph * 8 - 1);

        uint32_t entry = (nt + ((py >> 3) * pw + (px >> 3)) * 2) & 0xFFFF;
        uint16_t cell = (vram[entry] << 8) | vram[entry + 1];
        int tile = cell & 0x7FF;
        int row = (cell & 0x1000) ? 7 - (py & 7) : (py & 7);
        int col = (cell & 0x0800) ? 7 - (px & 7) : (px & 7);

        uint8_t b = vram[(tile * 32 + row * 4 + (col >> 1)) & 0xFFFF];
        uint8_t idx = (col & 1) ? (b & 0xF) : (b >> 4);
        if (!idx) continue;

        uint8_t rank = plane_b ? ((cell & 0x8000) ? 4 : 1)
                               : ((cell & 0x8000) ? 5 : 2);
        if (rank > line_rank[x]) {
            line_rank[x] = rank;
            line_idx[x] = ((cell >> 13) & 3) * 16 + idx;
        }
    }
}

static void render_window(int line, int width)
{
    int wx = (REG_WIN_X & 0x1F) * 16;
    int wy = (REG_WIN_Y & 0x1F) * 8;
    bool win_line = (REG_WIN_Y & 0x80) ? (line >= wy) : (line < wy);
    int x0, x1;
    if (win_line) { x0 = 0; x1 = width; }
    else if (REG_WIN_X & 0x80) { x0 = wx; x1 = width; }
    else { x0 = 0; x1 = wx; }
    if (x0 >= x1) return;

    /* window nametable: H40 ignores bit 1 */
    uint32_t nt = H40 ? ((REG_WINDOW & 0x3C) << 10) : ((REG_WINDOW & 0x3E) << 10);
    int wcells = H40 ? 64 : 32;

    for (int x = x0; x < x1; x++) {
        uint32_t entry = (nt + ((line >> 3) * wcells + (x >> 3)) * 2) & 0xFFFF;
        uint16_t cell = (vram[entry] << 8) | vram[entry + 1];
        int tile = cell & 0x7FF;
        int row = (cell & 0x1000) ? 7 - (line & 7) : (line & 7);
        int col = (cell & 0x0800) ? 7 - (x & 7) : (x & 7);
        uint8_t b = vram[(tile * 32 + row * 4 + (col >> 1)) & 0xFFFF];
        uint8_t idx = (col & 1) ? (b & 0xF) : (b >> 4);
        if (!idx) continue;
        uint8_t rank = (cell & 0x8000) ? 5 : 2;   /* same ranks as plane A */
        if (rank > line_rank[x]) {
            line_rank[x] = rank;
            line_idx[x] = ((cell >> 13) & 3) * 16 + idx;
        }
    }
}

static void render_sprites(int line, int width)
{
    uint32_t sat = (uint32_t)(REG_SPRITE & (H40 ? 0x7E : 0x7F)) << 9;
    int max_sprites = H40 ? 80 : 64;
    int max_line = H40 ? 20 : 16;
    int max_pixels = H40 ? 320 : 256;

    static uint8_t sp_idx[MD_SCREEN_W];
    static uint8_t sp_pri[MD_SCREEN_W];
    memset(sp_idx, 0, width);
    memset(sp_pri, 0, width);

    int sprite = 0, drawn = 0, pixels = 0;
    for (int n = 0; n < max_sprites; n++) {
        uint32_t e = (sat + sprite * 8) & 0xFFFF;
        int y = (((vram[e] << 8) | vram[e + 1]) & 0x3FF) - 128;
        int size = vram[e + 2];
        int link = vram[e + 3] & 0x7F;
        uint16_t attr = (vram[e + 4] << 8) | vram[e + 5];
        int x = (((vram[e + 6] << 8) | vram[e + 7]) & 0x1FF) - 128;

        int wc = ((size >> 2) & 3) + 1;   /* width in cells  */
        int hc = (size & 3) + 1;          /* height in cells */

        if (line >= y && line < y + hc * 8) {
            if (drawn >= max_line || pixels >= max_pixels)
                break;
            drawn++;
            pixels += wc * 8;

            int sy = line - y;
            if (attr & 0x1000) sy = hc * 8 - 1 - sy;           /* vflip */
            bool hflip = attr & 0x0800;
            int tile_base = attr & 0x7FF;
            uint8_t pal = (attr >> 13) & 3;
            uint8_t pri = (attr >> 15) & 1;

            for (int sx = 0; sx < wc * 8; sx++) {
                int dx = x + sx;
                if (dx < 0 || dx >= width) continue;
                if (sp_idx[dx]) continue;                      /* first sprite wins */
                int tx = hflip ? wc * 8 - 1 - sx : sx;
                int tile = tile_base + (tx >> 3) * hc + (sy >> 3);
                uint8_t b = vram[((tile & 0x7FF) * 32 + (sy & 7) * 4 + ((tx & 7) >> 1)) & 0xFFFF];
                uint8_t idx = (tx & 1) ? (b & 0xF) : (b >> 4);
                if (!idx) continue;
                sp_idx[dx] = pal * 16 + idx;
                sp_pri[dx] = pri;
            }
        }

        if (link == 0) break;
        sprite = link;
    }

    for (int x = 0; x < width; x++) {
        if (!sp_idx[x]) continue;
        uint8_t rank = sp_pri[x] ? 6 : 3;
        if (rank > line_rank[x]) {
            line_rank[x] = rank;
            line_idx[x] = sp_idx[x];
        }
    }
}

void vdp_render_line(int line)
{
    uint32_t *dst = md_fb + line * MD_SCREEN_W;
    uint32_t backdrop = cram_rgba[REG_BACKDROP & 0x3F];

    if (!DISPLAY_ON) {
        for (int x = 0; x < MD_SCREEN_W; x++) dst[x] = backdrop;
        return;
    }

    int width = H40 ? 320 : 256;
    uint8_t bd_idx = REG_BACKDROP & 0x3F;
    memset(line_rank, 0, width);
    memset(line_idx, bd_idx, width);

    render_plane(line, true, width);   /* B */
    render_plane(line, false, width);  /* A */
    render_window(line, width);
    render_sprites(line, width);

    int pad = (MD_SCREEN_W - width) / 2;
    for (int x = 0; x < pad; x++) dst[x] = backdrop;
    for (int x = 0; x < width; x++) dst[pad + x] = cram_rgba[line_idx[x]];
    for (int x = pad + width; x < MD_SCREEN_W; x++) dst[x] = backdrop;
}
