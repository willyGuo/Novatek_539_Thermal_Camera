#ifndef TIFFANY_DDE_H
#define TIFFANY_DDE_H
typedef struct {
    int enable;
    int sigma_s;
    int sigma_r;
    int gain_percent;
    int num_iters;
} DDE_PARAMS;
void dde_get_params(DDE_PARAMS *params);
int dde_set_params(const DDE_PARAMS *params);
int dde_validate_params(const DDE_PARAMS *params);
#endif
