#ifndef TIFFANY_IFE_PROBE_CORE_H
#define TIFFANY_IFE_PROBE_CORE_H

/* This is deliberately independent of HDAL and does not rescale the pixels. */
#define PROBE_W 640U
#define PROBE_H 480U
#define PROBE_PIXELS (PROBE_W * PROBE_H)
#define PROBE_BYTES (PROBE_PIXELS * 2U)

static unsigned short probe_pixel(unsigned int i, unsigned int pattern)
{
    static const unsigned short levels[] = {
        0, 1, 2, 3, 4094, 4095, 4096, 4097,
        8190, 8191, 8192, 8193, 12287, 12288, 16382, 16383
    };
    if (pattern == 1) return levels[(i / PROBE_W / 16U) % 16U];
    if (pattern == 2) {
        unsigned int x = i + 1U;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        return (unsigned short)(8192 + (int)(x % 65U) - 32);
    }
    return (unsigned short)(i % 16384U);
}

#endif
