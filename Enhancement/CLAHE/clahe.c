/* Adapted from Tiffany_thermal_CLAHE/Enhancement/CLAHE/clahe.c.
 * Preserve histogram redistribution and interpolation; add stride, validation,
 * persistent workspace and synchronized per-frame configuration snapshots. */
#include "clahe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
static pthread_mutex_t params_lock = PTHREAD_MUTEX_INITIALIZER;
/* Keep the existing image unchanged until CLAHE is explicitly enabled. */
static CLAHE_PARAMS current_params = {0, 32, 8};
static int valid_params(const CLAHE_PARAMS *p)
{
    return p && (p->enable == 0 || p->enable == 1) &&
        (p->tile_size == 8 || p->tile_size == 16 || p->tile_size == 32) &&
        p->clip_limit >= 4 && p->clip_limit <= 128;
}
void clahe_get_params(CLAHE_PARAMS *p)
{
    if (!p) return;
    pthread_mutex_lock(&params_lock); *p = current_params; pthread_mutex_unlock(&params_lock);
}
int clahe_set_params(const CLAHE_PARAMS *p)
{
    if (!valid_params(p)) return -1;
    pthread_mutex_lock(&params_lock); current_params = *p; pthread_mutex_unlock(&params_lock);
    return 0;
}
void clahe_workspace_release(CLAHE_WORKSPACE *w)
{
    if (!w) return;
    free(w->luts); w->luts = NULL; w->capacity = 0;
}
static void clipHistogram(uint32_t* hist, uint32_t nrBins, uint32_t clipLimit) {
    uint32_t excess = 0;

    for (uint32_t i = 0; i < nrBins; i++) {
        if (hist[i] > clipLimit) {
            excess += (hist[i] - clipLimit);
            hist[i] = clipLimit;
        }
    }

    uint32_t binIncr = excess / nrBins;
    uint32_t upperLimit = clipLimit - binIncr;

    for (uint32_t i = 0; i < nrBins; i++) {
        if (hist[i] <= upperLimit) {
            hist[i] += binIncr;
            excess -= binIncr;
        } else if (hist[i] < clipLimit) {
            excess -= (clipLimit - hist[i]);
            hist[i] = clipLimit;
        }
    }

    /* Public validation guarantees clipLimit * 256 >= tile pixel count. */
    while (excess > 0) {
        for (uint32_t i = 0; i < nrBins && excess > 0; i++) {
            if (hist[i] < clipLimit) {
                hist[i]++;
                excess--;
            }
        }
    }
}

static void makeLut(uint32_t* hist, uint32_t nrBins, uint16_t minVal, uint16_t maxVal, uint32_t nrPixels, uint16_t* lut) {
    uint32_t sum = 0;
    float scale = (float)(maxVal - minVal) / nrPixels;

    for (uint32_t i = 0; i < nrBins; i++) {
        sum += hist[i];
        uint32_t val = minVal + (uint32_t)(sum * scale);
        lut[i] = (val > maxVal) ? maxVal : val;
    }
}

static int clahe_core(uint8_t* src, uint32_t stride, uint32_t width, uint32_t height, uint32_t gridX, uint32_t gridY, uint32_t clipLimit, CLAHE_WORKSPACE *workspace)
{

    uint32_t nrBins = 256;
    uint32_t tileSizeX = width / gridX;
    uint32_t tileSizeY = height / gridY;
    uint32_t nrPixelsInTile = tileSizeX * tileSizeY;

    if (clipLimit < 1) clipLimit = 1;

    size_t required = (size_t)gridX * gridY * nrBins;
    if (required > workspace->capacity) {
        uint16_t *new_luts = realloc(workspace->luts, required * sizeof(uint16_t));
        if (!new_luts) return -2;
        workspace->luts = new_luts;
        workspace->capacity = required;
    }
    uint16_t *luts = workspace->luts;
    uint32_t tileHist[256];

    for (uint32_t gy = 0; gy < gridY; gy++) {
        for (uint32_t gx = 0; gx < gridX; gx++) {
            memset(tileHist, 0, nrBins * sizeof(uint32_t));

            uint32_t startX = gx * tileSizeX;
            uint32_t startY = gy * tileSizeY;

            for (uint32_t y = 0; y < tileSizeY; y++) {
                for (uint32_t x = 0; x < tileSizeX; x++) {
                    uint16_t pixelVal = src[(startY + y) * stride + (startX + x)];
                    tileHist[pixelVal]++;
                }
            }

            clipHistogram(tileHist, nrBins, clipLimit);

            uint16_t* currentLut = &luts[(gy * gridX + gx) * nrBins];
            makeLut(tileHist, nrBins, 0, 255, nrPixelsInTile, currentLut);
        }
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t pixelVal = src[y * stride + x];

            float tx = ((float)x - (float)tileSizeX / 2.0f) / (float)tileSizeX;
            float ty = ((float)y - (float)tileSizeY / 2.0f) / (float)tileSizeY;

            int gx0 = (int)tx;
            int gy0 = (int)ty;

            if (gx0 < 0) gx0 = 0;
            if (gx0 >= (int)gridX - 1) gx0 = gridX - 2;
            if (gy0 < 0) gy0 = 0;
            if (gy0 >= (int)gridY - 1) gy0 = gridY - 2;

            float fx = tx - gx0;
            float fy = ty - gy0;
            if(fx < 0) fx = 0;
            if(fx > 1) fx = 1;
            if(fy < 0) fy = 0;
            if(fy > 1) fy = 1;
            uint16_t v00 = luts[((gy0)     * gridX + (gx0))     * nrBins + pixelVal];
            uint16_t v10 = luts[((gy0)     * gridX + (gx0 + 1)) * nrBins + pixelVal];
            uint16_t v01 = luts[((gy0 + 1) * gridX + (gx0))     * nrBins + pixelVal];
            uint16_t v11 = luts[((gy0 + 1) * gridX + (gx0 + 1)) * nrBins + pixelVal];

            float res = (1.0f - fx) * (1.0f - fy) * v00 +
                        fx         * (1.0f - fy) * v10 +
                        (1.0f - fx) * fy         * v01 +
                        fx         * fy         * v11;

            src[y * stride + x] = (uint8_t)(res + 0.5f);
        }
    }


    return 0;
}
int clahe_apply_y8(uint8_t *image, uint32_t stride, uint32_t width,
                   uint32_t height, const CLAHE_PARAMS *p, CLAHE_WORKSPACE *w)
{
    if (!valid_params(p) || !w || !image) return -1;
    if (!p->enable) return 0;
    if (width > 4096 || height > 4096 || stride < width ||
        width < (uint32_t)p->tile_size * 2 || height < (uint32_t)p->tile_size * 2 ||
        width % p->tile_size || height % p->tile_size ||
        stride > UINT32_MAX / height) return -1;
    return clahe_core(image, stride, width, height, width / p->tile_size,
                      height / p->tile_size, p->clip_limit, w);
}
