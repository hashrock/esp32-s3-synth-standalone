#include "dsp/rhodes.h"
#include <cstdio>
int main(){rhodes::Bank<double> bank;bank.init(48000,4,true,rhodes::kBark);rhodes::BasicVoice<double> voice;voice.note_on(bank,60,78);for(int i=0;i<4096;i++)std::printf("%.17g\n",voice.process());}
