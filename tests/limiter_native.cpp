#include "dsp/limiter.h"
#include <cassert>
#include <cstdio>
int main() {
    dsp::LookaheadLimiter<64> limiter;
    limiter.init(48000);
    float block[64];
    // Below the ceiling, preserve the samples exactly after one block of delay.
    for(float &x:block)x=.25f;
    limiter.process(block,.25f);
    for(float x:block)assert(x==0);
    for(float &x:block)x=.5f;
    limiter.process(block,.5f);
    for(float x:block)assert(x==.25f);
    // Abrupt peaks at both block edges, positive and negative.
    for(int b=0;b<12;++b) {
        for(float &x:block)x=.2f;
        block[(b&1)?63:0]=(b&2)?16.f:-16.f;
        limiter.process(block,16);
        for(float x:block)assert(std::isfinite(x) && std::abs(x)<=.950001f);
    }
    // Recover smoothly after the transient and preserve low-level audio again.
    float previous=0;
    for(int b=0;b<1000;++b) {
        for(float &x:block)x=.1f;
        limiter.process(block,.1f);
        for(float x:block)assert(std::isfinite(x) && std::abs(x)<=.950001f);
        if(b>1)assert(block[63]>=previous-1e-6f);
        previous=block[63];
    }
    assert(previous>.09999f);
    std::puts("limiter: unity below ceiling, bounded transients, smooth recovery passed");
}
