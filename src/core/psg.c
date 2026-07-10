/* psg.c - SN76489 PSG (the discrete Texas Instruments part in the VDP) */
#include "core_internal.h"

static uint16_t tone[4];      /* reload values; [3] is the noise control reg */
static int32_t  counter[4];
static uint8_t  vol[4];       /* attenuation 0..15 (15 = silent) */
static uint8_t  out[4];       /* square wave phase */
static uint16_t lfsr;
static uint8_t  latch_ch, latch_vol;
static uint32_t tick_acc;

/* 2 dB attenuation steps, matched to BlastEm's calibration (32767/14 peak)
 * so PSG:FM balance is ~0.44 like measured hardware */
static const int16_t vol_tab[16] = {
    2340, 1859, 1477, 1173, 932, 740, 588, 469,
     371,  295,  234,  186, 148, 117,  93,   0
};

void psg_reset(void)
{
    for (int i = 0; i < 4; i++) {
        tone[i] = 0;
        counter[i] = 0;
        vol[i] = 15;
        out[i] = 1;
    }
    lfsr = 0x8000;
    latch_ch = 0;
    latch_vol = 0;
    tick_acc = 0;
}

void psg_write(uint8_t v)
{
    if (v & 0x80) {
        latch_ch = (v >> 5) & 3;
        latch_vol = (v >> 4) & 1;
        if (latch_vol) {
            vol[latch_ch] = v & 0xF;
        } else if (latch_ch == 3) {
            tone[3] = v & 7;
            lfsr = 0x8000;
        } else {
            tone[latch_ch] = (tone[latch_ch] & 0x3F0) | (v & 0xF);
        }
    } else {
        if (latch_vol) {
            vol[latch_ch] = v & 0xF;
        } else if (latch_ch == 3) {
            tone[3] = v & 7;
            lfsr = 0x8000;
        } else {
            tone[latch_ch] = (tone[latch_ch] & 0x00F) | ((v & 0x3F) << 4);
        }
    }
}

static void psg_tick(void)   /* one /16 divided clock (~223.7 kHz) */
{
    for (int ch = 0; ch < 3; ch++) {
        if (--counter[ch] <= 0) {
            counter[ch] = tone[ch] ? tone[ch] : 0x400;
            if (tone[ch] <= 1)
                out[ch] = 1;             /* inaudible rates act as DC (PCM trick) */
            else
                out[ch] ^= 1;
        }
    }
    if (--counter[3] <= 0) {
        switch (tone[3] & 3) {
        case 0: counter[3] = 0x10; break;
        case 1: counter[3] = 0x20; break;
        case 2: counter[3] = 0x40; break;
        case 3: counter[3] = tone[2] ? tone[2] : 0x400; break;
        }
        out[3] ^= 1;
        if (out[3]) {                    /* shift on rising edge */
            uint16_t fb = (tone[3] & 4)
                ? (uint16_t)(((lfsr ^ (lfsr >> 3)) & 1) << 15)   /* white */
                : (uint16_t)((lfsr & 1) << 15);                  /* periodic */
            lfsr = (lfsr >> 1) | fb;
        }
    }
}

static int16_t last_sample;

int16_t psg_sample(void)
{
    /* PSG tick rate 223721.5 Hz vs FM sample rate 53267 Hz: 4.2 ticks/sample */
    tick_acc += 275262;                  /* 4.2004 * 65536 */
    while (tick_acc >= 65536) {
        tick_acc -= 65536;
        psg_tick();
    }
    int32_t s = 0;
    for (int ch = 0; ch < 3; ch++)
        s += out[ch] ? vol_tab[vol[ch]] : -vol_tab[vol[ch]];
    s += (lfsr & 1) ? vol_tab[vol[3]] : -vol_tab[vol[3]];
    last_sample = (int16_t)s;
    return last_sample;
}

int16_t psg_sample_last(void) { return last_sample; }
