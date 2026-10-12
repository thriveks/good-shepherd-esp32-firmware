#pragma once
#include <math.h>
#include <stdint.h>

// Same protected-learning pattern as production LD2410: slow mean/deviation,
// warm-up, downweighted outliers, frozen trusted baseline and quarantined candidate.
// Thermal noise observations only. No person/fall/care classification or promotion.
class ThermalAdaptiveNoise {
 public:
  enum State { Learning, Normal, Protected, Candidate, Held, Unusable };
  void interrupt() { samples_ = 0; sum_ = 0; windowStarted_ = false; state_ = Unusable; }
  void reset() { *this = ThermalAdaptiveNoise{}; }
  void observe(float noise, uint32_t at, bool eligible) {
    if (!eligible || !isfinite(noise) || noise < 0 || noise > 50) { interrupt(); return; }
    if (!windowStarted_) { since_ = at; windowStarted_ = true; }
    sum_ += noise; ++samples_;
    if (uint32_t(at-since_) < 1000) return;
    const float value = sum_/samples_;
    const bool complete = samples_ >= 3 && uint32_t(at-since_) <= 1500;
    samples_ = 0; sum_ = 0; since_ = at;
    if (!complete) { state_ = Unusable; return; }
    if (!windows_) { mean_ = value; deviation_ = 0; windows_ = 1; state_ = Learning; return; }
    float alpha = 0.02f;
    const float score = fabsf(value-mean_)/fmaxf(deviation_, 0.03f);
    if (windows_ < 30) { alpha = 1.0f/float(windows_+1); state_ = Learning; }
    else if (score >= 8) {
      state_ = Candidate; ++frozen_;
      if (!candidateWindows_) candidateMean_ = value;
      else candidateMean_ += 0.02f*(value-candidateMean_);
      ++candidateWindows_;
      return; // Never promote or absorb this persistent unusual profile.
    } else if (score >= 4) { state_ = Protected; alpha = 0.002f; ++downweighted_; }
    else state_ = candidateWindows_ ? Held : Normal;
    float delta = value-mean_;
    mean_ += alpha*delta;
    deviation_ += alpha*(fabsf(delta)-deviation_);
    if (windows_ < 0xffffffffu) ++windows_;
  }
  bool ready() const { return windows_ >= 30; }
  float threshold() const { return fminf(1.5f, fmaxf(0.3f, mean_+4*fmaxf(deviation_,0.03f))); }
  float mean() const { return mean_; }
  float deviation() const { return deviation_; }
  float candidateMean() const { return candidateMean_; }
  uint32_t windows() const { return windows_; }
  uint32_t frozen() const { return frozen_; }
  uint32_t downweighted() const { return downweighted_; }
  State state() const { return state_; }
  const char* stateName() const {
    switch (state_) {
      case Learning: return "learning"; case Normal: return "normal_learning";
      case Protected: return "protected_learning"; case Candidate: return "persistent_candidate";
      case Held: return "candidate_held"; default: return "unusable_window";
    }
  }
 private:
  float mean_ = 0, deviation_ = 0, candidateMean_ = 0, sum_ = 0;
  uint32_t windows_ = 0, since_ = 0, candidateWindows_ = 0, frozen_ = 0, downweighted_ = 0;
  unsigned samples_ = 0;
  bool windowStarted_ = false;
  State state_ = Learning;
};
