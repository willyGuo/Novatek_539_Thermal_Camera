#ifndef TIFFANY_CLAHE_H
#define TIFFANY_CLAHE_H
#include <stdint.h>
#include <stddef.h>
typedef struct { int enable; int tile_size; int clip_limit; } CLAHE_PARAMS;
typedef struct { uint16_t *luts; size_t capacity; } CLAHE_WORKSPACE;
void clahe_get_params(CLAHE_PARAMS *params);
int clahe_set_params(const CLAHE_PARAMS *params);
int clahe_apply_y8(uint8_t *image, uint32_t stride, uint32_t width,
                   uint32_t height, const CLAHE_PARAMS *params, CLAHE_WORKSPACE *workspace);
void clahe_workspace_release(CLAHE_WORKSPACE *workspace);
#endif
