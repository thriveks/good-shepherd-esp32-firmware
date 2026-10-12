#include "thermal_learning_summary.h"
#include <assert.h>
#include <algorithm>
#include <iostream>
int main() {
  ThermalLocalFeatures local; float frame[768];
  for(unsigned n=1;n<=520;n++) {
    std::fill(frame,frame+768,n%2?25.4f:24.6f);
    for(unsigned y=8;y<12;y++)for(unsigned x=4;x<8;x++)frame[y*32+x]=30;
    for(unsigned y=8;y<12;y++)for(unsigned x=20;x<24;x++)frame[y*32+x]=30;
    local.process(frame,768,n*250,n);
  }
  assert(local.result().noiseReady && local.result().ambiguous);
  char small[8];assert(!thermalLearningSummary(small,sizeof(small),local.result(),local.noise(),"version",0,0));
  char json[768];assert(thermalLearningSummary(json,sizeof(json),local.result(),local.noise(),
    "esp32-good-shepherd-thermal-v0.6.0-learning-status",0,1));
  std::cout<<json<<"\n";
}
