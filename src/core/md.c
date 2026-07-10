/* md.c - machine: 68k memory map, Musashi glue, scanline scheduler, audio ring */
#include <stdio.h>
#include <string.h>
#include "core_internal.h"
#include "m68k.h"

uint8_t  md_rom[0x400000];
uint32_t md_rom_size;
uint8_t  md_ram[0x10000];
uint8_t  md_sram[0x10000];          /* battery RAM at $200000, odd bytes */
uint32_t md_fb[MD_SCREEN_W * MD_SCREEN_H];
int      md_line;
int      md_line_68k_cycles;

static uint64_t instr_count;

/* ------------------------------------------------------------- debugging */
#define MAX_BREAKS 8
static struct { uint32_t pc; const char *label; bool hit; } breaks[MAX_BREAKS];
static int n_breaks;

void md_add_break_pc(uint32_t pc, const char *label)
{
    if (n_breaks < MAX_BREAKS) {
        breaks[n_breaks].pc = pc;
        breaks[n_breaks].label = label;
        breaks[n_breaks].hit = false;
        n_breaks++;
    }
}

static void instr_hook(unsigned int pc)
{
    instr_count++;
    for (int i = 0; i < n_breaks; i++) {
        if (!breaks[i].hit && breaks[i].pc == pc) {
            breaks[i].hit = true;
            printf("[break] PC=%06X (%s) after %llu instructions, line %d\n",
                   pc, breaks[i].label ? breaks[i].label : "?",
                   (unsigned long long)instr_count, md_line);
            fflush(stdout);
        }
    }
}

uint32_t md_get_pc(void)      { return m68k_get_reg(NULL, M68K_REG_PC); }
uint64_t md_instr_count(void) { return instr_count; }

/* ------------------------------------------------------------ 68k bus map
 * $000000-$3FFFFF ROM
 * $200000-$20FFFF SRAM (odd bytes) when the ROM doesn't reach that high
 * $A00000-$A0FFFF Z80 address space (when 68k owns the bus)
 * $A10000-$A1001F I/O (version reg, pads)
 * $A11100 Z80 BUSREQ   $A11200 Z80 RESET   $A14000 TMSS (ignored)
 * $C00000-$C0000F VDP (data/control/HV), $C00011 PSG
 * $E00000-$FFFFFF work RAM, mirrored every 64 KB
 */

static inline bool is_sram(uint32_t a)
{
    return md_rom_size <= 0x200000 && a >= 0x200000 && a < 0x210000;
}

unsigned int m68k_read_memory_8(unsigned int a)
{
    a &= 0xFFFFFF;
    if (a < 0x400000) {
        if (is_sram(a)) return (a & 1) ? md_sram[(a - 0x200000) >> 1] : 0xFF;
        return a < md_rom_size ? md_rom[a] : 0xFF;
    }
    if (a >= 0xE00000) return md_ram[a & 0xFFFF];
    if ((a & 0xFF0000) == 0xA00000) return z80bus_read8_from68k(a);
    if ((a & 0xFFFFE0) == 0xA10000) return io_read(a);
    if ((a & 0xFFFF00) == 0xA11100) return z80bus_busreq_read() >> ((a & 1) ? 0 : 8);
    if ((a & 0xFFFFFC) == 0xC00000) return (uint8_t)(vdp_read_data() >> ((a & 1) ? 0 : 8));
    if ((a & 0xFFFFFC) == 0xC00004) return (uint8_t)(vdp_read_status() >> ((a & 1) ? 0 : 8));
    if ((a & 0xFFFFFC) == 0xC00008) return (uint8_t)(vdp_read_hv() >> ((a & 1) ? 0 : 8));
    return 0xFF;
}

unsigned int m68k_read_memory_16(unsigned int a)
{
    a &= 0xFFFFFE;
    if (a < 0x400000) {
        if (is_sram(a)) return 0xFF00 | md_sram[(a - 0x200000) >> 1];
        if (a + 1 < md_rom_size) return (md_rom[a] << 8) | md_rom[a + 1];
        return 0xFFFF;
    }
    if (a >= 0xE00000) {
        uint32_t i = a & 0xFFFF;
        return (md_ram[i] << 8) | md_ram[i + 1];
    }
    if ((a & 0xFF0000) == 0xA00000) {
        uint8_t b = z80bus_read8_from68k(a);
        return (b << 8) | b;
    }
    if ((a & 0xFFFFE0) == 0xA10000) {
        uint8_t b = io_read(a | 1);
        return (b << 8) | b;
    }
    if ((a & 0xFFFF00) == 0xA11100) return z80bus_busreq_read();
    if ((a & 0xFFFFFC) == 0xC00000) return vdp_read_data();
    if ((a & 0xFFFFFC) == 0xC00004) return vdp_read_status();
    if ((a & 0xFFFFFC) == 0xC00008) return vdp_read_hv();
    return 0xFFFF;
}

unsigned int m68k_read_memory_32(unsigned int a)
{
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}

void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    a &= 0xFFFFFF;
    v &= 0xFF;
    if (a >= 0xE00000) { md_ram[a & 0xFFFF] = v; return; }
    if (is_sram(a)) { if (a & 1) md_sram[(a - 0x200000) >> 1] = v; return; }
    if ((a & 0xFF0000) == 0xA00000) { z80bus_write8_from68k(a, v); return; }
    if ((a & 0xFFFFE0) == 0xA10000) { io_write(a, v); return; }
    if ((a & 0xFFFF00) == 0xA11100) { z80bus_busreq_write((v << 8) | v); return; }
    if ((a & 0xFFFF00) == 0xA11200) { z80bus_reset_write((v << 8) | v); return; }
    if ((a & 0xFFFFFC) == 0xC00000) { vdp_write_data((v << 8) | v); return; }
    if ((a & 0xFFFFFC) == 0xC00004) { vdp_write_control((v << 8) | v); return; }
    if (a == 0xC00011 || a == 0xC00013) { psg_write(v); return; }
    /* TMSS ($A14000) and anything else: ignore */
}

void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    a &= 0xFFFFFE;
    v &= 0xFFFF;
    if (a >= 0xE00000) {
        uint32_t i = a & 0xFFFF;
        md_ram[i] = v >> 8;
        md_ram[i + 1] = v & 0xFF;
        return;
    }
    if (is_sram(a)) { md_sram[(a - 0x200000) >> 1] = v & 0xFF; return; }
    if ((a & 0xFF0000) == 0xA00000) { z80bus_write8_from68k(a, v >> 8); return; }
    if ((a & 0xFFFFE0) == 0xA10000) { io_write(a | 1, v & 0xFF); return; }
    if ((a & 0xFFFF00) == 0xA11100) { z80bus_busreq_write(v); return; }
    if ((a & 0xFFFF00) == 0xA11200) { z80bus_reset_write(v); return; }
    if ((a & 0xFFFFFC) == 0xC00000) { vdp_write_data(v); return; }
    if ((a & 0xFFFFFC) == 0xC00004) { vdp_write_control(v); return; }
    if (a == 0xC00010 || a == 0xC00012) { psg_write(v & 0xFF); return; }
}

void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v & 0xFFFF);
}

/* disassembler bus (side-effect-free reads would be better; ROM/RAM only) */
unsigned int m68k_read_disassembler_8(unsigned int a)  { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ------------------------------------------------------------- interrupts */
static int int_ack(int level)
{
    vdp_int_ack(level);
    return M68K_INT_ACK_AUTOVECTOR;
}

/* ------------------------------------------------------------------ audio
 * Native-rate (~53.267 kHz) stereo ring buffer filled by the scheduler,
 * drained by the frontend with linear resampling.
 */
#define ARING_SIZE 32768  /* frames, power of two */
static int16_t aring[ARING_SIZE * 2];
static uint32_t aring_w;
static double resample_pos;

void md_audio_push(int16_t l, int16_t r)
{
    uint32_t w = aring_w % ARING_SIZE;
    aring[w * 2] = l;
    aring[w * 2 + 1] = r;
    aring_w++;
}

int md_read_audio(int16_t *dst, int max_frames, int out_rate)
{
    const double step = 53267.0 / (double)out_rate;
    if (resample_pos + ARING_SIZE < (double)aring_w)   /* fell too far behind */
        resample_pos = (double)aring_w - ARING_SIZE / 2;
    int n = 0;
    while (n < max_frames) {
        uint32_t base = (uint32_t)resample_pos;
        if (base + 1 >= aring_w) break;       /* need two source frames */
        double frac = resample_pos - (double)base;
        uint32_t i0 = (base % ARING_SIZE) * 2;
        uint32_t i1 = ((base + 1) % ARING_SIZE) * 2;
        dst[n * 2]     = (int16_t)(aring[i0]     + frac * (aring[i1]     - aring[i0]));
        dst[n * 2 + 1] = (int16_t)(aring[i0 + 1] + frac * (aring[i1 + 1] - aring[i0 + 1]));
        resample_pos += step;
        n++;
    }
    return n;
}

/* -------------------------------------------------------------- lifecycle */
bool md_load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    md_rom_size = (uint32_t)fread(md_rom, 1, sizeof md_rom, f);
    fclose(f);
    return md_rom_size >= 0x200;
}

void md_reset(void)
{
    memset(md_ram, 0, sizeof md_ram);
    memset(md_fb, 0, sizeof md_fb);
    vdp_reset();
    io_reset();
    psg_reset();
    z80bus_reset();

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(int_ack);
    m68k_set_instr_hook_callback(instr_hook);
    m68k_pulse_reset();

    md_line = 0;
    instr_count = 0;
    md_cycle_base = 0;
}

const uint32_t *md_framebuffer(void) { return md_fb; }
void md_set_pad(int pad, uint16_t buttons) { io_set_pad(pad, buttons); }

/* --------------------------------------------------------------- schedule */
uint64_t md_cycle_base;
int      md_line_cycles = M68K_CYCLES_LINE;
static int master_acc;   /* fractional master-clock remainder (3420/7) */

void md_run_frame(void)
{
    for (md_line = 0; md_line < MD_LINES_TOTAL; md_line++) {
        vdp_line_start(md_line);

        master_acc += 3420;
        md_line_cycles = master_acc / 7;   /* 488 or 489, avg 488.571 */
        master_acc -= md_line_cycles * 7;

        md_line_68k_cycles = 0;
        m68k_execute(md_line_cycles);
        md_line_68k_cycles = md_line_cycles;

        z80bus_run_line();
        z80bus_ym_catchup(md_cycle_base + md_line_cycles);

        if (md_line < MD_LINES_ACTIVE)
            vdp_render_line(md_line);

        if (md_line == MD_LINES_ACTIVE - 1)
            z80bus_vint();   /* Z80 INT is asserted at start of vblank */

        md_cycle_base += md_line_cycles;
    }
}

