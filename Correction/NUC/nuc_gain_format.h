#ifndef NUC_GAIN_FORMAT_H
#define NUC_GAIN_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define NUC_GAIN_SHIFT 8U
#define NUC_GAIN_ONE (1U << NUC_GAIN_SHIFT)
/* GFX interprets K as signed INT16; reject gains that would become negative. */
#define NUC_GAIN_MAX 32767U

/* Validate Q8 K without rescaling; B remains signed DN. */
static inline int nuc_gain_table_validate_q8(const uint16_t *table, size_t count)
{
    size_t i;
    if (table == NULL || count == 0) return -1;
    for (i = 0; i < count; i++) {
        if (table[i] == 0 || table[i] > NUC_GAIN_MAX) return -1;
    }
    return 0;
}

#endif
