#ifndef NUC_GAIN_FORMAT_H
#define NUC_GAIN_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define NUC_GAIN_SHIFT 14U
#define NUC_GAIN_ONE (1U << NUC_GAIN_SHIFT)
#define NUC_GAIN_MAX 32767U
#define NUC_LEGACY_GAIN_SHIFT 8U

/* Validate the whole table first: no partial conversion on invalid input.
 * B is signed DN and must never be rescaled with K. */
static inline int nuc_gain_table_to_q14(uint16_t *table, size_t count,
                                      unsigned int source_shift)
{
    size_t i;
    unsigned int scale;
    if (table == NULL || count == 0 ||
        (source_shift != NUC_GAIN_SHIFT && source_shift != NUC_LEGACY_GAIN_SHIFT))
        return -1;
    scale = 1U << (NUC_GAIN_SHIFT - source_shift);
    for (i = 0; i < count; i++) {
        if (table[i] == 0 || table[i] > NUC_GAIN_MAX / scale) return -1;
    }
    for (i = 0; i < count; i++) table[i] = (uint16_t)(table[i] * scale);
    return 0;
}

#endif
