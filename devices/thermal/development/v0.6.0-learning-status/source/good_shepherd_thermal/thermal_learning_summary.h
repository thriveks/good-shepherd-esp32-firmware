#pragma once
#include <stdio.h>
#include "thermal_local_features.h"

// Bounded, aggregate-only status added to the existing once-per-minute summary.
// No pixels, regions, person labels or alert decisions are sent here.
inline bool thermalLearningSummary(char* output, size_t capacity,
    const ThermalLocalFeatures::Result& r, const ThermalAdaptiveNoise& noise,
    const char* version, uint32_t errors, uint32_t gaps) {
  int size = snprintf(output, capacity,
    "{\"schemaVersion\":1,\"softwareVersion\":\"%s\",\"baselineReady\":%s,"
    "\"emptySceneConfirmed\":%s,\"noiseReady\":%s,\"learningState\":\"%s\","
    "\"learningWindows\":%lu,\"backgroundPixels\":%u,\"eligible\":%s,"
    "\"captureSeq\":%lu,\"captureAtMs\":%lu,\"readErrors\":%lu,\"samplingGaps\":%lu,"
    "\"noiseC\":%.3f,\"stillnessThresholdC\":%.3f}",
    version, r.baselineReady ? "true":"false", r.calibrated ? "true":"false",
    r.noiseReady ? "true":"false", noise.stateName(), (unsigned long)noise.windows(),
    unsigned(r.noiseBackgroundPixels), r.noiseEligible ? "true":"false",
    (unsigned long)r.sequence, (unsigned long)r.captureMs, (unsigned long)errors,
    (unsigned long)gaps, r.learnedNoiseC, r.stillnessThresholdC);
  return size > 0 && size_t(size) < capacity;
}
