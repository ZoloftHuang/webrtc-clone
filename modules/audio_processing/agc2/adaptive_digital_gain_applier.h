/*
 *  Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_AUDIO_PROCESSING_AGC2_ADAPTIVE_DIGITAL_GAIN_APPLIER_H_
#define MODULES_AUDIO_PROCESSING_AGC2_ADAPTIVE_DIGITAL_GAIN_APPLIER_H_

#include "modules/audio_processing/agc2/gain_applier.h"
#include "modules/audio_processing/include/audio_frame_view.h"
#include "modules/audio_processing/vad/vad_with_level.h"

namespace webrtc {

class ApmDataDumper;

class AdaptiveDigitalGainApplier {
 public:
  explicit AdaptiveDigitalGainApplier(ApmDataDumper* apm_data_dumper);
  // Decide what gain to apply.
  void Process(
      float input_level_dbfs,
      float input_noise_level_dbfs,
      rtc::ArrayView<const VadWithLevel::LevelAndProbability> vad_results,
      AudioFrameView<float> float_frame);

 private:
  float last_gain_db_ = 0.f;
  GainApplier gain_applier_;

  // For some combinations of noise and speech probability, increasing
  // the level is not allowed. Since we may get VAD results in bursts,
  // we keep track of this variable until the next VAD results come
  // in.
  bool gain_increase_allowed_ = true;
  ApmDataDumper* apm_data_dumper_ = nullptr;
};
}  // namespace webrtc

#endif  // MODULES_AUDIO_PROCESSING_AGC2_ADAPTIVE_DIGITAL_GAIN_APPLIER_H_
