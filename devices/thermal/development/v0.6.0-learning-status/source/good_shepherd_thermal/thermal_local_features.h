#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include "thermal_adaptive_noise.h"

// Original bounded implementation of background subtraction / connected regions.
// No trained model: a warm region is NOT a person, posture, fall or body temperature.
// Call only with two complementary calibrated MLX90640 subpages, 768 pixels.
class ThermalLocalFeatures {
 public:
  static constexpr unsigned Pixels = 768, MaxRegions = 8, CalibrationFrames = 32;
  struct Region {
    uint16_t area = 0;
    uint8_t x0 = 31, y0 = 23, x1 = 0, y1 = 0;
    float cx = 0, cy = 0, contrast = 0, temporalChangeC = 0;
  };
  struct Result {
    bool valid = false, calibrated = false, continuous = false, ambiguous = false;
    bool tracked = false, stationaryCandidate = false, baselineReady = false, noiseReady = false;
    bool noiseEligible = false;
    uint16_t noiseBackgroundPixels = 0;
    float learnedNoiseC = 0, learnedDeviationC = 0, stillnessThresholdC = 0.3f;
    uint8_t regions = 0;
    uint32_t captureMs = 0, sequence = 0, dwellMs = 0;
    float minC = 0, maxC = 0, meanC = 0, changeC = 0;
    Region region[MaxRegions];
  };
  void calibrateEmptyScene() {
    // User must independently confirm an empty, fixed, thermally settled scene.
    calibrated_ = false; backgroundReady_ = false; explicitEmpty_ = true; calibrating_ = true; calibrationCount_ = 0;
    resetTrack();
  }
  void interrupt() { havePrevious_ = false; resetTrack(); noise_.interrupt();
          if (calibrating_) calibrationCount_ = 0; }
  const ThermalAdaptiveNoise& noise() const { return noise_; }
  const Result& result() const { return out_; }
  uint32_t rejected() const { return rejected_; }
  uint32_t gaps() const { return gaps_; }
  void reject() { ++rejected_; interrupt(); out_ = Result{}; }
  const Result& process(const float* frame, unsigned count, uint32_t at, uint32_t seq) {
    out_ = Result{};
    if (!frame || count != Pixels) { reject(); return out_; }
    float lo = INFINITY, hi = -INFINITY, sum = 0, change = 0;
    for (unsigned i = 0; i < Pixels; ++i) {
      if (!isfinite(frame[i]) || frame[i] < -40 || frame[i] > 300) { reject(); return out_; }
      lo = fminf(lo, frame[i]); hi = fmaxf(hi, frame[i]); sum += frame[i];
      if (havePrevious_) change += fabsf(frame[i] - previous_[i]);
    }
    if (haveTimestamp_ && uint32_t(at-lastAt_) > 30000) {
      const bool explicitRequested = calibrating_ && explicitEmpty_;
      calibrated_ = false; backgroundReady_ = false; explicitEmpty_ = explicitRequested;
      calibrating_ = true; calibrationCount_ = 0; noise_.reset();
    }
    bool continuous = havePrevious_ && uint32_t(at - lastAt_) > 0 &&
      uint32_t(at - lastAt_) <= 1000 && uint32_t(seq - lastSeq_) == 1;
    if (havePrevious_ && !continuous) {
      ++gaps_; resetTrack();
      if (calibrating_) calibrationCount_ = 0;
      // Long pauses restart the provisional scene baseline.
      if (uint32_t(at - lastAt_) > 30000) calibrated_ = false;
    }
    out_.valid = true; out_.captureMs = at; out_.sequence = seq;
    out_.minC = lo; out_.maxC = hi; out_.meanC = sum / Pixels;
    out_.changeC = havePrevious_ ? change / Pixels : 0;
    out_.continuous = continuous;
    if (calibrating_) {
      if (explicitEmpty_ && calibrationCount_ && out_.changeC > 0.5f) calibrationCount_ = 0;
      if (!calibrationCount_ && !explicitEmpty_) {
        // Provisional bootstrap: majority-scene estimate, never an empty-room claim.
        // Seed initially warm pixels below their observed value rather than absorbing
        // a stationary person/object present at boot. Occupied/low-contrast scenes
        // can still be unresolved; semantic classifiers stay unknown.
        for (unsigned i = 0; i < Pixels; ++i) queue_[i] = i;
        std::sort(queue_, queue_+Pixels, [frame](uint16_t a, uint16_t b) { return frame[a] < frame[b]; });
        const float median = (frame[queue_[383]] + frame[queue_[384]])*0.5f;
        for (unsigned i = 0; i < Pixels; ++i)
          background_[i] = frame[i] >= median+2.0f ? median+0.5f : frame[i];
      } else {
        for (unsigned i = 0; i < Pixels; ++i) {
          if (!explicitEmpty_ && frame[i]-background_[i] >= 2.0f) continue;
          background_[i] = calibrationCount_ ? background_[i] +
            (frame[i]-background_[i])/float(calibrationCount_+1) : frame[i];
        }
      }
      if (++calibrationCount_ == CalibrationFrames) {
        calibrated_ = explicitEmpty_; backgroundReady_ = true; calibrating_ = false;
      }
    } else if (backgroundReady_) {
      float backgroundChange = 0; unsigned backgroundSamples = 0;
      for (unsigned i = 0; i < Pixels; ++i) {
        mask_[i] = frame[i] - background_[i] >= 2.0f;
        // Exclude current AND previous foreground; entering/leaving objects
        // cannot train the trusted thermal-noise baseline.
        if (continuous && !mask_[i] && fabsf(frame[i]-background_[i]) < 1.0f &&
            fabsf(previous_[i]-background_[i]) < 1.0f) {
          backgroundChange += fabsf(frame[i]-previous_[i]); ++backgroundSamples;
        }
      }
      const float observedNoise = backgroundSamples ? backgroundChange/backgroundSamples : 0;

      // Four-connected flood fill. At most 768 queue entries; each pixel enqueued once.
      for (unsigned seed = 0; seed < Pixels; ++seed) {
        if (!mask_[seed]) continue;
        unsigned head = 0, tail = 0; queue_[tail++] = seed; mask_[seed] = 0;
        Region region; float sx = 0, sy = 0, contrast = 0, regionChange = 0;
        while (head < tail) {
          unsigned i = queue_[head++], x = i % 32, y = i / 32;
          ++region.area; sx += x; sy += y; contrast += frame[i] - background_[i];
          if (continuous) regionChange += fabsf(frame[i]-previous_[i]);
          if (x < region.x0) region.x0 = x;
          if (x > region.x1) region.x1 = x;
          if (y < region.y0) region.y0 = y;
          if (y > region.y1) region.y1 = y;
          if (x) enqueue(i-1, tail);
          if (x < 31) enqueue(i+1, tail);
          if (y) enqueue(i-32, tail);
          if (y < 23) enqueue(i+32, tail);
        }
        if (region.area < 4) continue; // Isolated hot pixels never create a track.
        region.cx = sx / region.area; region.cy = sy / region.area;
        region.contrast = contrast / region.area;
        region.temporalChangeC = continuous ? regionChange/region.area : 0;
        if (out_.regions < MaxRegions) out_.region[out_.regions++] = region;
        else out_.ambiguous = true;
      }
      out_.ambiguous = out_.ambiguous || out_.regions > 1;
      for (unsigned i = 0; i < out_.regions; ++i)
        if (out_.region[i].area > Pixels/2) out_.ambiguous = true; // Scene-wide change/occlusion.
      // Noise eligibility comes from background pixels, not the number of warm
      // regions. Multiple heaters/people must not prevent room-noise learning.
      // Still require a continuous frame and at least half the scene confirmed
      // near background in BOTH frames. Tracking remains conservative below.
      out_.noiseBackgroundPixels = backgroundSamples;
      out_.noiseEligible = continuous && backgroundSamples >= Pixels/2;
      noise_.observe(observedNoise, at, out_.noiseEligible);
      // Never absorb warm foreground, including a stationary region, into background.
      // Adapt only non-warm pixels at a time-based rate (~5 minute time constant).
      if (continuous) {
        float alpha = float(uint32_t(at-lastAt_)) / 300000.0f;
        for (unsigned i = 0; i < Pixels; ++i)
          if (frame[i] - background_[i] < 2.0f && fabsf(frame[i]-background_[i]) < 1.0f)
            background_[i] += alpha * (frame[i] - background_[i]);
      }
      if (out_.regions == 1 && !out_.ambiguous) {
        const Region& r = out_.region[0];
        float dx = r.cx - lastCx_, dy = r.cy - lastCy_;
        const bool same = tracking_ && continuous && dx*dx+dy*dy <= 9.0f &&
          r.area <= lastArea_*2 && r.area*2 >= lastArea_;
        if (!same) { anchorX_ = r.cx; anchorY_ = r.cy; dwellStart_ = at; }
        float ax = r.cx-anchorX_, ay = r.cy-anchorY_;
        // Compare against a fixed anchor: slow drift must not count as immobility.
        if (ax*ax+ay*ay > 1.0f || r.temporalChangeC > noise_.threshold() || !noise_.ready() ||
            noise_.state() == ThermalAdaptiveNoise::Candidate || noise_.state() == ThermalAdaptiveNoise::Unusable) {
          anchorX_ = r.cx; anchorY_ = r.cy; dwellStart_ = at;
        }
        out_.tracked = same;
        out_.dwellMs = at-dwellStart_;
        out_.stationaryCandidate = same && noise_.ready() && out_.dwellMs >= 60000;
        tracking_ = true; lastCx_ = r.cx; lastCy_ = r.cy; lastArea_ = r.area;
      } else resetTrack();
    }
    out_.calibrated = calibrated_;
    out_.baselineReady = backgroundReady_; out_.noiseReady = noise_.ready();
    out_.learnedNoiseC = noise_.mean(); out_.learnedDeviationC = noise_.deviation();
    out_.stillnessThresholdC = noise_.threshold();
    memcpy(previous_, frame, sizeof(previous_));
    havePrevious_ = true; haveTimestamp_ = true; lastAt_ = at; lastSeq_ = seq;
    return out_;
  }
 private:
  void enqueue(unsigned i, unsigned& tail) { if (mask_[i]) { mask_[i] = 0; queue_[tail++] = i; } }
  void resetTrack() { tracking_ = false; dwellStart_ = 0; }
  float background_[Pixels] = {}, previous_[Pixels] = {};
  uint8_t mask_[Pixels] = {};
  uint16_t queue_[Pixels] = {};
  Result out_;
  ThermalAdaptiveNoise noise_;
  bool backgroundReady_ = false, explicitEmpty_ = false;
  bool calibrated_ = false, calibrating_ = true, havePrevious_ = false, haveTimestamp_ = false, tracking_ = false;
  unsigned calibrationCount_ = 0, lastArea_ = 0;
  uint32_t lastAt_ = 0, lastSeq_ = 0, dwellStart_ = 0, rejected_ = 0, gaps_ = 0;
  float lastCx_ = 0, lastCy_ = 0, anchorX_ = 0, anchorY_ = 0;
};

// Network cadence is independent of acquisition and lease renewal.
struct ThermalImageCadence {
  bool sent = false; uint32_t last = 0;
  bool due(uint32_t at) const { return !sent || uint32_t(at-last) >= 1000; }
  void attempted(uint32_t at) { sent = true; last = at; }
};
