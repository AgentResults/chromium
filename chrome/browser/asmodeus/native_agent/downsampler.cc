// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/downsampler.h"

namespace asmodeus {

void Downsampler48to16::Process(const float* in_48k,
                                int n_in,
                                std::vector<float>& out_16k) {
  // 3:1 decimation with simple averaging (low-pass).
  for (int i = 0; i + 2 < n_in; i += 3) {
    out_16k.push_back((in_48k[i] + in_48k[i + 1] + in_48k[i + 2]) / 3.0f);
  }
}

}  // namespace asmodeus
