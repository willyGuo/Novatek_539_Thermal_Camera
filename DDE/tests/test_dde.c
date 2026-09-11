#include "../dde_dt_opt.h"
#include "../dde.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    const unsigned dims[][2] = {{1,1}, {1,19}, {19,1}, {17,23}, {640,512}};
    DDE_PARAMS p, previous;
    dde_get_params(&p);
    assert(!p.enable && p.num_iters == 3);
    p.enable = 1;
    p.num_iters = 1;
    assert(dde_set_params(&p) == 0);
    previous = p;
    p.num_iters = 0;
    assert(dde_set_params(&p) != 0);
    dde_get_params(&p);
    assert(p.num_iters == previous.num_iters);
    for (unsigned k = 0; k < sizeof(dims)/sizeof(dims[0]); ++k) {
        unsigned w = dims[k][0], h = dims[k][1], stride = w + 7;
        unsigned size = dde_dt_get_workbuf_size(w,h);
        unsigned char *work = malloc(size + 32), *img = malloc(stride*h + 32);
        assert(work && img);
        memset(work + size, 0xA5, 32);
        for (int n = 1; n <= 3; n += 2) {
            memset(img, 0xCC, stride*h + 32);
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++)
                img[y*stride+x] = (unsigned char)((x*13+y*7)%256);
            assert(DDE_DT_Optimized(img,w,h,stride,8,16,1,n,255,0,work)==0);
            for (unsigned y=0;y<h;y++) {
                for (unsigned x=0;x<w;x++) assert(img[y*stride+x]==(x*13+y*7)%256);
                for (unsigned x=w;x<stride;x++) assert(img[y*stride+x]==0xCC);
            }
            assert(DDE_DT_Optimized(img,w,h,stride,8,16,10,n,200,20,work)==0);
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++)
                assert(img[y*stride+x]>=20 && img[y*stride+x]<=200);
            for (unsigned i=0;i<32;i++) {
                assert(work[size+i]==0xA5);
                assert(img[stride*h+i]==0xCC);
            }
            memset(img, 77, stride*h);
            assert(DDE_DT_Optimized(img,w,h,stride,8,16,1.5f,n,255,0,work)==0);
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) assert(img[y*stride+x]==77);
        }
        assert(DDE_DT_Optimized(img,w,h,w-1,8,16,1,1,255,0,work)!=0);
        assert(DDE_DT_Optimized(img,w,h,stride,NAN,16,1,1,255,0,work)!=0);
        assert(DDE_DT_Optimized(img,w,h,stride,8,INFINITY,1,1,255,0,work)!=0);
        free(work); free(img);
    }
    assert(dde_dt_get_workbuf_size(0,512)==0);
    assert(dde_dt_get_workbuf_size(0xffffffffU,512)==0);
    puts("PASS: Y8 identity, flat field, clipping, padded stride, work bounds, invalid parameters, runtime settings");
    return 0;
}
