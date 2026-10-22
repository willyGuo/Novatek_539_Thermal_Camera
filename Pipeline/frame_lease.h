#ifndef FRAME_LEASE_H
#define FRAME_LEASE_H

#include "hdal.h"
#include "frame_diagnostics.h"
#include <stdio.h>
#include <string.h>

/* One owner thread while running; Stop may retry only after joining it. */
typedef struct {
    HD_VIDEO_FRAME frame;
    int held;
    unsigned long long pulled, released, release_errors;
} FRAME_LEASE;

typedef HD_RESULT (*FRAME_PULL_FN)(HD_PATH_ID, HD_VIDEO_FRAME *, INT32);
typedef HD_RESULT (*FRAME_RELEASE_FN)(HD_PATH_ID, HD_VIDEO_FRAME *);

static inline HD_RESULT frame_lease_pull(FRAME_LEASE *lease,
    FRAME_PULL_FN pull, HD_PATH_ID path, HD_VIDEO_FRAME *frame, INT32 wait_ms)
{
    HD_RESULT ret;
    if (lease->held) return HD_ERR_NG;
    ret = pull(path, frame, wait_ms);
    if (ret == HD_OK) {
        lease->frame = *frame;
        lease->held = 1;
        lease->pulled++;
    }
    return ret;
}

static inline HD_RESULT frame_lease_release(FRAME_LEASE *lease,
    FRAME_RELEASE_FN release, HD_PATH_ID path)
{
    HD_RESULT ret;
    HD_VIDEO_FRAME descriptor;
    if (!lease->held) return HD_OK;
    /* Keep the original even if an unsuccessful SDK call modifies its input. */
    descriptor = lease->frame;
    ret = release(path, &descriptor);
    if (ret == HD_OK) {
        lease->held = 0;
        lease->released++;
    } else {
        lease->release_errors++;
    }
    return ret;
}

static inline void frame_lease_report(const char *name, const FRAME_LEASE *lease)
{
    printf("%s ownership: pull=%llu release=%llu held=%d release_error=%llu\n",
        name, lease->pulled, lease->released, lease->held, lease->release_errors);
}
#endif
