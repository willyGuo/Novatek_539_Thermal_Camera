#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "candidate/agc_weighted.c"
AGC_MODE agc_mode_get(void) { return AGC_MODE_WEIGHTED_CPU; }
static void fill(uint16_t *raw, unsigned cold, unsigned background)
{
    unsigned i;
    for(i=0;i<10000;i++) raw[i]=i<cold ? 3800 : i<cold+background ? 4200 : 4800;
}
int main(void)
{
    AGC_WEIGHTED_WORKSPACE *ws=calloc(1,sizeof(*ws)), *fresh=calloc(1,sizeof(*fresh));
    AGC_WEIGHTED_PARAMS p=AGC_WEIGHTED_DEFAULT_PARAMS;
    uint16_t *raw=malloc(20000), *out=malloc(20000), *ref=malloc(20000);
    unsigned i; double delta=0; int confident; double center;
    assert(ws&&fresh&&raw&&out&&ref);
    p.balanced_background=1;p.temporal_previous_percent=0;p.gain_limit_tenths=5;
    fill(raw,1000,8000);
    assert(!agc_weighted_apply_u14(raw,200,out,200,100,100,ws,&p));
    printf("three groups 10/80/10: center=%.2f DN (expected dominant4200)\n",ws->balanced_center_q8/256.0);
    fill(raw,8000,1000);
    assert(!agc_weighted_apply_u14(raw,200,out,200,100,100,ws,&p));
    assert(!agc_weighted_apply_u14(raw,200,ref,200,100,100,fresh,&p));
    for(i=0;i<10000;i++) delta+=abs((int)out[i]-(int)ref[i])*255.0/16383/10000;
    printf("Smooth0 identical second input: reused_center=%.2f fresh_center=%.2f mean_output_difference=%.3f Gray\n",
        ws->balanced_center_q8/256.0,fresh->balanced_center_q8/256.0,delta);
    assert(ws->balanced_center_q8!=fresh->balanced_center_q8 && delta>1);
    agc_weighted_state_reset(ws);
    memset(ws->histogram,0,sizeof(ws->histogram));ws->histogram[3800]=1000;ws->histogram[4200]=9000;
    center=balanced_background_center(ws,10000,&confident);
    printf("two groups cold10/background90: center=%.2f DN (still lowest group)\n",center);
    assert(center==3800);
    free(ws);free(fresh);free(raw);free(out);free(ref);return 0;
}
