#ifndef FRAME_DIAGNOSTICS_H
#define FRAME_DIAGNOSTICS_H

/* Temporarily quiet frame-count/timestamp test output. Compile with
 * -DFRAME_DIAGNOSTICS_ENABLED=1 to restore it. Ownership and RAW input
 * completion protection remain active; release/timeout errors still print. */
#ifndef FRAME_DIAGNOSTICS_ENABLED
#define FRAME_DIAGNOSTICS_ENABLED 0
#endif

#endif
