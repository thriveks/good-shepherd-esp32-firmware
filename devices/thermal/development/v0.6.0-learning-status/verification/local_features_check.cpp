#include "thermal_local_features.h"
#include "thermal_stream_policy.h"
#include <assert.h>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <random>
static float frame[768];
static uint32_t at = 0, seq = 0;
static void empty() { std::fill(frame, frame+768, 25.f); }
static void blob(int x,int y,int w=4,int h=4,float temp=30) {
  for(int j=y;j<y+h;j++) for(int i=x;i<x+w;i++) frame[j*32+i]=temp;
}
static const ThermalLocalFeatures::Result& next(ThermalLocalFeatures& f) {
 at+=250; return f.process(frame,768,at,++seq);
}
static void calibrate(ThermalLocalFeatures& f) {
 empty(); f.calibrateEmptyScene();
 for(unsigned i=0;i<32;i++) next(f);
 assert(f.result().calibrated && !f.result().regions);
}
int main() {
 ThermalLocalFeatures f;
 empty(); blob(8,8); next(f);
 assert(f.result().valid && !f.result().calibrated && !f.result().regions);
 calibrate(f);
 blob(8,8); next(f); auto r=next(f);
 assert(r.regions==1 && r.region[0].area==16 && r.region[0].cx==9.5f && r.tracked);
 assert(r.region[0].x0==8 && r.region[0].x1==11);
 for(int i=0;i<400;i++) next(f);
 assert(f.result().stationaryCandidate && f.result().dwellMs>=60000);
 // Stationary foreground must survive >background time constant.
 for(int i=0;i<2400;i++) next(f);
 assert(f.result().regions==1 && f.result().stationaryCandidate);
 empty();blob(12,8);next(f);assert(!f.result().stationaryCandidate && f.result().dwellMs==0);
 // Slow one-pixel steps must reset the fixed anchor, despite track association.
 for(int x=12;x<22;x++){empty();blob(x,8); for(int j=0;j<40;j++)next(f);}
 assert(!f.result().stationaryCandidate);
 blob(2,2);next(f);assert(f.result().ambiguous && !f.result().tracked && f.result().dwellMs==0);
 empty();blob(8,8);next(f);assert(!f.result().tracked);
 at+=2000; next(f);assert(!f.result().continuous && !f.result().tracked && f.gaps()>0);
 next(f);assert(f.result().tracked);
 frame[557]=NAN;next(f);assert(!f.result().valid && f.rejected()==1);
 empty();blob(8,8);next(f);assert(!f.result().tracked && !f.result().continuous);
 f.process(frame,767,at,seq);assert(!f.result().valid);
 frame[0]=301;next(f);assert(!f.result().valid);
 empty(); frame[0]=INFINITY;next(f);assert(!f.result().valid);
 // Single hot pixel and many disconnected regions cannot create a lone-person track.
 empty();frame[0]=70;next(f);assert(f.result().regions==0 && !f.result().tracked);
 empty();for(int y=0;y<24;y+=4)for(int x=0;x<32;x+=4)blob(x,y,2,2);
 next(f);assert(f.result().regions==8 && f.result().ambiguous && !f.result().tracked);
 // Whole-scene warm changes must not overflow the fixed flood queue.
 std::fill(frame,frame+768,28.f);next(f);assert(f.result().region[0].area==768 && f.result().ambiguous && !f.result().tracked);
 // Duplicate and skipped capture sequence invalidate dwell; uint32 wrap remains valid.
 empty();blob(8,8);next(f); f.process(frame,768,at+250,seq);
 assert(!f.result().continuous && !f.result().tracked);
 seq+=10;next(f);assert(!f.result().continuous);
 ThermalLocalFeatures wrap;const uint32_t savedAt=at; at=0xffffdf00u;calibrate(wrap);at=savedAt;
 empty();blob(8,8);wrap.process(frame,768,0xfffffff0u,0xffffffffu);
 auto wr=wrap.process(frame,768,0x000000eau,0u);assert(wr.continuous && wr.tracked);
 at+=31000;next(f);assert(!f.result().calibrated);
 calibrate(f); f.interrupt(); at+=31000; next(f); assert(!f.result().calibrated);
 // A moving scene cannot finish empty-scene calibration.
 ThermalLocalFeatures moving;moving.calibrateEmptyScene();
 for(int i=0;i<100;i++){std::fill(frame,frame+768,i%2?25.f:27.f);next(moving);}
 assert(!moving.result().calibrated);
 // Cancellation, expiry and renewal never override the independent 1Hz image cap.
 ThermalStreamPolicy lease;ThermalImageCadence image;
 lease.renew(0xfffffff0u);assert(lease.active(10));assert(!lease.active(15000));
 image.attempted(0xfffffff0u);assert(!image.due(10));assert(image.due(1000));
 image.attempted(1000);lease.renew(1100);assert(!image.due(1100));
 lease.cancel();assert(!lease.active(1101));
 // Automatic bootstrap preserves an initially stationary warm region and never
 // asserts that the room was empty. Learned noise tolerates sensor-like changes
 // above the old fixed 0.3C whole-frame reset threshold.
 ThermalLocalFeatures automatic;
 for(int n=0;n<520;n++) {
   const float jitter=n%2 ? 0.4f : -0.4f;
   std::fill(frame,frame+768,25.f+jitter);blob(8,8,4,4,30.f+jitter);
   next(automatic);
   if(n==31) assert(automatic.result().baselineReady && !automatic.result().calibrated);
 }
 assert(automatic.result().regions==1 && automatic.result().noiseReady);
 assert(automatic.result().changeC>0.3f && automatic.result().stationaryCandidate);
 assert(automatic.noise().mean()>0.7f && automatic.noise().threshold()>0.8f);
 empty();blob(16,8);next(automatic);assert(!automatic.result().stationaryCandidate);
 // Protected learning mirrors presence policy; persistent noise is quarantined.
 ThermalAdaptiveNoise learned;uint32_t clock=0;learned.observe(0.4f,clock,true);
 auto window=[&](float value){for(int i=0;i<4;i++){clock+=250;learned.observe(value,clock,true);}};
 for(int i=0;i<40;i++)window(0.4f);
 assert(learned.ready() && fabsf(learned.mean()-0.4f)<0.001f);
 window(0.55f);window(0.55f);assert(learned.downweighted()>0);
 float trusted=learned.mean();for(int i=0;i<60;i++)window(2.0f);
 assert(learned.frozen()>0 && learned.state()==ThermalAdaptiveNoise::Candidate);
 assert(fabsf(learned.mean()-trusted)<0.001f && learned.candidateMean()>1.9f);
 window(0.4f);window(0.4f);assert(learned.state()==ThermalAdaptiveNoise::Held);
 auto before=learned.windows();learned.observe(NAN,clock+250,true);
 assert(learned.state()==ThermalAdaptiveNoise::Unusable && learned.windows()==before);
 learned.interrupt();learned.observe(0.4f,clock+500,false);assert(learned.windows()==before);
 // Regression: several warm regions must not stall noise learning when a
 // majority of stable background pixels is available. Never grant a track.
 for(int regions=2;regions<=4;regions++) {
   ThermalLocalFeatures multiple;
   for(int n=0;n<520;n++) {
     float jitter=n%2 ? 0.4f : -0.4f;
     std::fill(frame,frame+768,25.f+jitter);
     for(int k=0;k<regions;k++)blob(2+7*k,8,4,4,30.f+jitter);
     next(multiple);
     if(n>32) {
       assert(multiple.result().regions==regions && multiple.result().ambiguous);
       assert(multiple.result().noiseEligible && multiple.result().noiseBackgroundPixels==768-16*regions);
       assert(!multiple.result().tracked && multiple.result().dwellMs==0);
       assert(!multiple.result().stationaryCandidate);
     }
   }
   assert(multiple.result().noiseReady && multiple.noise().windows()>=30);
   assert(!multiple.result().calibrated && multiple.noise().mean()>0.7f);
 }
 // Moving warm foreground is excluded in BOTH frames; its temperature change
 // cannot raise the trusted background noise even with multiple regions.
 ThermalLocalFeatures transitioning;calibrate(transitioning);
 for(int n=0;n<200;n++) {
   empty();blob(n%2?8:4,8,4,4,n%2?40:30);blob(22,8);
   next(transitioning);
 }
 assert(transitioning.noise().ready() && transitioning.noise().mean()==0);
 assert(transitioning.result().ambiguous && !transitioning.result().tracked);
 // Exactly half the scene may learn; less than half / whole-scene occlusion
 // must interrupt learning without changing the trusted profile.
 ThermalLocalFeatures coverage;calibrate(coverage);
 empty();blob(0,0,16,24);next(coverage);next(coverage);
 assert(coverage.result().noiseBackgroundPixels==384 && coverage.result().noiseEligible);
 auto noiseWindows=coverage.noise().windows();
 for(int n=0;n<200;n++){empty();blob(0,0,17,24);next(coverage);}
 assert(coverage.result().noiseBackgroundPixels==360 && !coverage.result().noiseEligible);
 assert(coverage.noise().windows()==noiseWindows && !coverage.result().noiseReady);
 assert(coverage.result().ambiguous && !coverage.result().stationaryCandidate);
 std::fill(frame,frame+768,30.f);next(coverage);
 assert(coverage.result().noiseBackgroundPixels==0 && !coverage.result().noiseEligible);
 auto heldWindows=transitioning.noise().windows();
 frame[0]=NAN;next(transitioning);
 assert(!transitioning.result().valid && transitioning.noise().windows()==heldWindows);
 empty();blob(8,8);at+=2000;next(transitioning);
 assert(!transitioning.result().noiseEligible && !transitioning.result().stationaryCandidate);
 // Deterministic noise/boundary stress under ASan/UBSan, including all pixels warm.
 std::mt19937 rng(90640);std::uniform_real_distribution<float> noise(-0.2f,6.f);
 calibrate(f);auto begin=std::chrono::steady_clock::now();
 for(int n=0;n<10000;n++) {for(float& v:frame)v=25+noise(rng);next(f);assert(f.result().regions<=8);}
 double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
 std::cout<<"PASS: multi-region learning regression, foreground exclusion, background coverage guards, validity, explicit calibration, regions, tracking, dwell, gaps, ambiguity, wrap, lease/cadence; 10000 stress frames in "<<ms<<" ms; core bytes "<<sizeof(f)<<"\n";
}
