/* z80bus.c - Z80 side: sound RAM, BUSREQ/RESET, bank window, YM2612
 *
 * The YM2612 uses catch-up scheduling: whenever anyone touches the chip we
 * first clock it up to that bus master's current cycle, so write spacing,
 * DAC sample timing and timer flag reads all see real timing instead of
 * per-scanline batches.
 */
#include <stdio.h>
#include <string.h>
#include "core_internal.h"
#include "z80.h"
#include "ym3438.h"
#include "m68k.h"

static z80      cpu;
static ym3438_t ym;
static uint8_t  zram[0x2000];
static bool     busreq;   /* 68k has requested the bus (Z80 halted) */
static bool     zreset;   /* Z80 held in reset */
static uint16_t bank;     /* 9-bit page for the $8000-$FFFF window */

static uint32_t ym_writes, ym_keyons;
static uint32_t ym_status_reads, ym_r27_writes;
static uint8_t  ym_last_status;
static int      fm_peak;

/* ------------------------------------------------------ YM2612 catch-up
 * fm_pos: chip position on the 68k-cycle timeline. One OPN2_Clock is one
 * internal chip cycle = 6 68k cycles; 24 of them make one output sample.
 */
static uint64_t fm_pos;
static int      fm_sub;            /* 0..23 within the current sample */
static int32_t  acc_l, acc_r;
static float    lpf_l, lpf_r;
static bool     z80_slice;         /* inside z80bus_run_line stepping */

/* Model-1 style first-order low-pass (~3.4 kHz at the 53.267 kHz native rate) */
#define LPF_ALPHA 0.33f

static void ym_clock_once(void)
{
    Bit16s buf[2];
    OPN2_Clock(&ym, buf);
    fm_pos += 6;
    acc_l += buf[0];
    acc_r += buf[1];
    if (++fm_sub == 24) {
        fm_sub = 0;
        /* FM gain x7 puts one full channel at ~5370, matching BlastEm's
         * YM scale (0x1FE0 * 79/120) so absolute loudness is comparable */
        int32_t l = acc_l * 7 + psg_sample();
        int32_t r = acc_r * 7 + psg_sample_last();
        acc_l = acc_r = 0;
        if (l > fm_peak) fm_peak = l;
        lpf_l += LPF_ALPHA * ((float)l - lpf_l);
        lpf_r += LPF_ALPHA * ((float)r - lpf_r);
        int32_t ol = (int32_t)lpf_l, or_ = (int32_t)lpf_r;
        if (ol > 32767) ol = 32767; else if (ol < -32768) ol = -32768;
        if (or_ > 32767) or_ = 32767; else if (or_ < -32768) or_ = -32768;
        md_audio_push((int16_t)ol, (int16_t)or_);
    }
}

void z80bus_ym_catchup(uint64_t target)
{
    while (fm_pos + 6 <= target)
        ym_clock_once();
}

/* The write latch needs a few chip clocks to be consumed; writes issued
 * back-to-back by either CPU must not clobber it, so nudge time forward. */
static uint64_t bus_now(void);
static uint64_t ym_last_write_pos;
static int      ym_gap_clocks = 4;

/* DAC rate meter: measures the actual reg-0x2A write rate */
static uint64_t dac_first_pos, dac_last_pos;
static uint32_t dac_count;

static void ym_write_raw(uint8_t port, uint8_t data)
{
    z80bus_ym_catchup(bus_now());
    while (fm_pos < ym_last_write_pos + (uint64_t)ym_gap_clocks * 6)
        ym_clock_once();
    if ((port & 1) && ym.address == 0x2A) {
        if (!dac_count) dac_first_pos = fm_pos;
        dac_last_pos = fm_pos;
        dac_count++;
    }
    OPN2_Write(&ym, port, data);
    ym_last_write_pos = fm_pos;
    /* real chip: 32 busy clocks after a data write; address writes are quick */
    ym_gap_clocks = (port & 1) ? 32 : 4;
}

/* Our scanline scheduler interleaves 68k and Z80 write batches in a way the
 * real machine (BUSREQ-serialized) does not, so an (address, data) pair from
 * one CPU can be split by the other re-aiming the address latch - data then
 * lands in the wrong register (phantom key-ons etc). Keep a shadow address
 * per CPU and transparently re-issue it when the latch was stolen. */
static uint8_t  addr_owner[2];          /* per part: 0 = 68k, 1 = z80 */
static uint8_t  shadow_addr[2][2];      /* [origin][part] */
static uint32_t hazard_count;           /* healed cross-CPU splits */

static void ym_write_paced(uint8_t port, uint8_t data)
{
    uint8_t origin = z80_slice ? 1 : 0;
    uint8_t part = (port >> 1) & 1;
    if (!(port & 1)) {
        shadow_addr[origin][part] = data;
        addr_owner[part] = origin;
    } else if (addr_owner[part] != origin) {
        hazard_count++;
        ym_write_raw(port & 2, shadow_addr[origin][part]);
        addr_owner[part] = origin;
    }
    ym_write_raw(port, data);
}

/* current position of whoever is touching the bus, in 68k cycles */
static unsigned long z80_line_cyc0;
static unsigned bank_penalty_acc;  /* fractional wait-state accumulator */

static uint64_t bus_now(void)
{
    if (z80_slice)
        return md_cycle_base + ((uint64_t)(cpu.cyc - z80_line_cyc0) * 15) / 7;
    uint64_t c = (uint64_t)m68k_cycles_run();
    if (c > M68K_CYCLES_LINE) c = M68K_CYCLES_LINE;
    return md_cycle_base + c;
}

/* ------------------------------------------------- Z80-visible memory map */
static uint8_t zmem_read(void *ud, uint16_t a)
{
    (void)ud;
    if (a < 0x4000) return zram[a & 0x1FFF];
    if (a < 0x6000) {
        z80bus_ym_catchup(bus_now());
        ym_status_reads++;
        ym_last_status = OPN2_Read(&ym, a & 3);
        return ym_last_status;
    }
    if (a < 0x8000) return 0xFF;    /* bank reg / VDP area: write-only */
    uint32_t target = ((uint32_t)bank << 15) | (a & 0x7FFF);
    if ((target & 0xFF0000) == 0xA00000) return 0xFF;  /* no self-recursion */
    if (z80_slice) {
        /* 68k-bus arbitration stalls the Z80 per access (~3.3 documented
         * minimum average; we use 4.2 to also cover unmodeled DMA/refresh
         * contention - ear-calibrated against real hardware / BlastEm);
         * without this, DAC loops streaming from ROM run audibly sharp */
        bank_penalty_acc += 42;
        cpu.cyc += bank_penalty_acc / 10;
        bank_penalty_acc %= 10;
    }
    return (uint8_t)m68k_read_memory_8(target);
}

static void zmem_write(void *ud, uint16_t a, uint8_t v)
{
    (void)ud;
    if (a < 0x4000) { zram[a & 0x1FFF] = v; return; }
    if (a < 0x6000) {
        ym_writes++;
        if ((a & 3) == 0 && v == 0x28) ym_keyons++;
        if ((a & 3) == 0 && v == 0x27) ym_r27_writes++;
        ym_write_paced(a & 3, v);
        return;
    }
    if (a < 0x6100) { bank = (uint16_t)(((bank >> 1) | ((v & 1) << 8)) & 0x1FF); return; }
    if (a >= 0x7F11 && a <= 0x7F17 && (a & 1)) { psg_write(v); return; }
    /* banked window writes and the rest: ignored */
}

static uint8_t zport_in(z80 *z, uint8_t port)  { (void)z; (void)port; return 0xFF; }
static void    zport_out(z80 *z, uint8_t port, uint8_t v) { (void)z; (void)port; (void)v; }

/* ------------------------------------------------------------- 68k side */
uint8_t z80bus_read8_from68k(uint32_t addr)
{
    if (!busreq) return 0xFF;       /* Z80 owns its bus */
    return zmem_read(NULL, addr & 0xFFFF);
}

void z80bus_write8_from68k(uint32_t addr, uint8_t v)
{
    if (!busreq) return;
    zmem_write(NULL, addr & 0xFFFF, v);
}

uint16_t z80bus_busreq_read(void)
{
    /* bit 8 low = bus granted to the 68k */
    return (busreq && !zreset) ? 0x0000 : 0x0100;
}

/* BUSREQ stall accounting: the Z80 only loses the actual assert->release
 * span (real pauses are microseconds), not whole scanline slices. */
static uint64_t busreq_since;    /* when the current assert began */
static uint64_t stall_68k;       /* stalled 68k-cycles accumulated this line */
static unsigned z80_frac;        /* fractional Z80 cycles carried (x7/15) */

void z80bus_busreq_write(uint16_t v)
{
    bool req = (v & 0x100) != 0;
    if (req && !busreq)
        busreq_since = bus_now();
    if (!req && busreq) {
        uint64_t now = bus_now();
        if (now > busreq_since)
            stall_68k += now - busreq_since;
    }
    busreq = req;
}

void z80bus_reset_write(uint16_t v)
{
    bool release = (v & 0x100) != 0;
    if (!release && !zreset) {      /* assert reset: YM2612 resets too */
        z80bus_ym_catchup(bus_now());
        OPN2_Reset(&ym);
    }
    if (release && zreset) {        /* leaving reset: Z80 starts at 0 */
        z80_init(&cpu);
        cpu.read_byte = zmem_read;
        cpu.write_byte = zmem_write;
        cpu.port_in = zport_in;
        cpu.port_out = zport_out;
    }
    zreset = !release;
}

/* ------------------------------------------------------------ lifecycle */
void z80bus_reset(void)
{
    memset(zram, 0, sizeof zram);
    busreq = true;                  /* 68k owns the bus after power-on */
    zreset = true;
    bank = 0;
    z80_init(&cpu);
    cpu.read_byte = zmem_read;
    cpu.write_byte = zmem_write;
    cpu.port_in = zport_in;
    cpu.port_out = zport_out;
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&ym);
    fm_pos = 0;
    fm_sub = 0;
    acc_l = acc_r = 0;
    lpf_l = lpf_r = 0;
    ym_writes = ym_keyons = 0;
}

void z80bus_run_line(void)
{
    uint64_t line_end = md_cycle_base + md_line_cycles;

    /* close out this line's stall bookkeeping */
    uint64_t stall = stall_68k;
    stall_68k = 0;
    if (busreq) {
        uint64_t from = busreq_since > md_cycle_base ? busreq_since : md_cycle_base;
        if (line_end > from)
            stall += line_end - from;
        busreq_since = line_end;   /* carry the assert into the next line */
    }

    if (zreset) return;

    uint64_t avail = (uint64_t)md_line_cycles > stall
                   ? (uint64_t)md_line_cycles - stall : 0;
    unsigned budget = ((unsigned)avail * 7 + z80_frac) / 15;
    z80_frac = ((unsigned)avail * 7 + z80_frac) % 15;
    if (budget == 0) return;

    z80_slice = true;
    z80_line_cyc0 = cpu.cyc;
    unsigned long target = cpu.cyc + budget;
    while (cpu.cyc < target)
        z80_step(&cpu);
    z80_slice = false;
}

void z80bus_vint(void)
{
    if (!zreset)
        z80_gen_int(&cpu, 0xFF);
}

/* Program a maximum-volume algorithm-7 patch on channel 1, key it on, and
 * measure the peak over ~0.2s. Isolates chip usage from the game's driver. */
int z80bus_ym_selftest(void)
{
    static const uint8_t seq[][2] = {
        {0x22,0x00},{0x27,0x00},{0x28,0x00},
        {0x30,0x01},{0x34,0x01},{0x38,0x01},{0x3C,0x01},   /* DT/MUL   */
        {0x40,0x00},{0x44,0x00},{0x48,0x00},{0x4C,0x00},   /* TL max   */
        {0x50,0x1F},{0x54,0x1F},{0x58,0x1F},{0x5C,0x1F},   /* AR 31    */
        {0x60,0x00},{0x64,0x00},{0x68,0x00},{0x6C,0x00},
        {0x70,0x00},{0x74,0x00},{0x78,0x00},{0x7C,0x00},
        {0x80,0x0F},{0x84,0x0F},{0x88,0x0F},{0x8C,0x0F},   /* SL0 RR15 */
        {0x90,0x00},{0x94,0x00},{0x98,0x00},{0x9C,0x00},
        {0xB0,0x07},{0xB4,0xC0},                           /* alg7, LR */
        {0xA4,0x22},{0xA0,0x69},                           /* f-num    */
        {0x28,0xF0},                                       /* key on   */
    };
    uint64_t t = fm_pos;
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        OPN2_Write(&ym, 0, seq[i][0]);
        t += 60; z80bus_ym_catchup(t);
        OPN2_Write(&ym, 1, seq[i][1]);
        t += 60; z80bus_ym_catchup(t);
    }
    fm_peak = 0;
    z80bus_ym_catchup(t + 144 * 10000);
    return fm_peak;
}

void z80bus_debug(char *buf, int n)
{
    double dac_hz = 0;
    if (dac_count > 1 && dac_last_pos > dac_first_pos)
        dac_hz = (double)(dac_count - 1) * 7670454.0
               / (double)(dac_last_pos - dac_first_pos);
    snprintf(buf, n,
             "pc=%04X ym=%u keyon=%u hazards=%u sval=%02X fmpeak=%d dacHz=%.1f (n=%u)",
             cpu.pc, ym_writes, ym_keyons, hazard_count,
             ym_last_status, fm_peak, dac_hz, dac_count);
    fm_peak = 0;
    dac_count = 0;
}
