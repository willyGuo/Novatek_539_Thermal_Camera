#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "agc_weighted.h"
#include "agc_mode.h"
AGC_MODE agc_mode_get(void) { return AGC_MODE_WEIGHTED_CPU; }
int main(void)
{
    AGC_WEIGHTED_WORKSPACE *ws=calloc(1,sizeof(*ws)), *fresh=calloc(1,sizeof(*fresh));
    AGC_WEIGHTED_PARAMS p=AGC_WEIGHTED_DEFAULT_PARAMS, instant;
    uint16_t raw[420], out[420], expected[420];
    int frame,i,first_end=0,last_bad=0; double max_error=0;
    p.clip_tenths=80;p.bright_weight=2;p.bright_percentile_tenths=800;
    p.gain_limit_tenths=5;p.temporal_previous_percent=50;p.balanced_background=1;
    instant=p;instant.temporal_previous_percent=0;
    agc_weighted_begin_reacquire(ws);
    puts("frame,input_center,tracked_center,reacquire_remaining,mean_gray_error");
    for(frame=0;frame<450;frame++) {
        int offset=frame<8 ? 380 : 0;
        double error=0;
        for(i=0;i<420;i++)raw[i]=(uint16_t)(4490+i%21+offset);
        if(agc_weighted_apply_u14(raw,42,out,42,21,20,ws,&p))return 1;
        agc_weighted_state_reset(fresh);
        if(agc_weighted_apply_u14(raw,42,expected,42,21,20,fresh,&instant))return 1;
        for(i=0;i<420;i++)error+=fabs((double)out[i]-expected[i])*255/16383/420;
        if(error>max_error)max_error=error;
        if(error>1)last_bad=frame;
        if(!ws->reacquire_remaining&&!first_end)first_end=frame+1;
        printf("%d,%d,%.3f,%u,%.4f\n",frame+1,4500+offset,ws->balanced_center_q8/256.0,ws->reacquire_remaining,error);
    }
    fprintf(stderr,"warmup_ended_frame=%d max_gray_error=%.3f last_error_above_1gray_frame=%d at30fps=%.3fs\n",first_end,max_error,last_bad+1,(last_bad+1)/30.0);
    free(ws);free(fresh);return 0;
}
