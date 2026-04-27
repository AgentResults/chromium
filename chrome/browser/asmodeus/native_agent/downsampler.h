// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_DOWNSAMPLER_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_DOWNSAMPLER_H_

#include <vector>

namespace asmodeus {

// Simple 3:1 decimation from 48kHz to 16kHz.
// Uses averaging over 3 samples as a basic low-pass filter.
class Downsampler48to16 {
 public:
  Downsampler48to16() = default;

  // Process n_in samples at 48kHz, append n_in/3 samples at 16kHz to out.
  void Process(const float* in_48k, int n_in, std::vector<float>& out_16k);
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_DOWNSAMPLER_H_
