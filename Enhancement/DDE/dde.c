#include "dde.h"
#include <pthread.h>
static pthread_mutex_t params_lock = PTHREAD_MUTEX_INITIALIZER;
static DDE_PARAMS current_params = {0, 8, 16, 150, 2, 1};
int dde_validate_params(const DDE_PARAMS *p)
{
    return p && (p->enable == 0 || p->enable == 1) &&
        p->sigma_s >= 1 && p->sigma_s <= 256 &&
        p->sigma_r >= 1 && p->sigma_r <= 255 &&
        p->gain_percent >= 0 && p->gain_percent <= 1000 &&
        p->num_iters >= 1 && p->num_iters <= 8 &&
        (p->base_scale == 1 || p->base_scale == 2) ? 0 : -1;
}
void dde_get_params(DDE_PARAMS *p)
{
    if (!p) return;
    pthread_mutex_lock(&params_lock);
    *p = current_params;
    pthread_mutex_unlock(&params_lock);
}
int dde_set_params(const DDE_PARAMS *p)
{
    if (dde_validate_params(p) != 0) return -1;
    pthread_mutex_lock(&params_lock);
    current_params = *p;
    pthread_mutex_unlock(&params_lock);
    return 0;
}
