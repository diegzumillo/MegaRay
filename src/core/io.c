/* io.c - version register and controller ports (3-button pads) */
#include "core_internal.h"

/* $A10001 version: overseas, NTSC, no expansion, hardware version 0 (no TMSS) */
#define VERSION_REG 0xA0

static uint8_t  ctrl[3];      /* direction masks   ($A10009/B/D) */
static uint8_t  data_out[3];  /* last written data ($A10003/5/7) */
static uint16_t pad_state[2]; /* MD_BTN_* bitmasks, active high  */

void io_reset(void)
{
    for (int i = 0; i < 3; i++) { ctrl[i] = 0; data_out[i] = 0; }
    pad_state[0] = pad_state[1] = 0;
}

void io_set_pad(int pad, uint16_t buttons)
{
    if (pad >= 0 && pad < 2) pad_state[pad] = buttons;
}

static uint8_t pad_read(int i)
{
    uint16_t b = i < 2 ? pad_state[i] : 0;
    /* TH: driven value if configured as output, otherwise pulled up */
    bool th = (ctrl[i] & 0x40) ? (data_out[i] & 0x40) : true;

    uint8_t v;
    if (th) {
        v = 0x40;
        if (!(b & MD_BTN_UP))    v |= 0x01;
        if (!(b & MD_BTN_DOWN))  v |= 0x02;
        if (!(b & MD_BTN_LEFT))  v |= 0x04;
        if (!(b & MD_BTN_RIGHT)) v |= 0x08;
        if (!(b & MD_BTN_B))     v |= 0x10;
        if (!(b & MD_BTN_C))     v |= 0x20;
    } else {
        v = 0x00;
        if (!(b & MD_BTN_UP))    v |= 0x01;
        if (!(b & MD_BTN_DOWN))  v |= 0x02;
        if (!(b & MD_BTN_A))     v |= 0x10;
        if (!(b & MD_BTN_START)) v |= 0x20;
    }
    /* output-configured bits read back the written value */
    return (uint8_t)((v & ~ctrl[i]) | (data_out[i] & ctrl[i]));
}

uint8_t io_read(uint32_t addr)
{
    switch (addr & 0x1F) {
    case 0x01: return VERSION_REG;
    case 0x03: return pad_read(0);
    case 0x05: return pad_read(1);
    case 0x07: return pad_read(2);
    case 0x09: return ctrl[0];
    case 0x0B: return ctrl[1];
    case 0x0D: return ctrl[2];
    default:   return 0xFF;   /* serial regs etc. */
    }
}

void io_write(uint32_t addr, uint8_t v)
{
    switch (addr & 0x1F) {
    case 0x03: data_out[0] = v; break;
    case 0x05: data_out[1] = v; break;
    case 0x07: data_out[2] = v; break;
    case 0x09: ctrl[0] = v; break;
    case 0x0B: ctrl[1] = v; break;
    case 0x0D: ctrl[2] = v; break;
    default: break;
    }
}
