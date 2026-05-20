// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_ASMODEUS_AUDIO_CAPTURE_H_
#define CHROME_BROWSER_ASMODEUS_ASMODEUS_AUDIO_CAPTURE_H_

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "content/public/browser/web_contents.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"

namespace content {
class WebContents;
}

namespace asmodeus {

// Captures all audio playing in a WebContents tab and writes to a WAV file.
// Uses JavaScript AudioContext + ScriptProcessor injection for capture
// (reliable, uses Chrome's tested Web Audio path) combined with C++ file
// writing for output.
class AsmodeusAudioCapture {
 public:
  AsmodeusAudioCapture();
  ~AsmodeusAudioCapture();

  AsmodeusAudioCapture(const AsmodeusAudioCapture&) = delete;
  AsmodeusAudioCapture& operator=(const AsmodeusAudioCapture&) = delete;

  // Start capturing audio from the given WebContents.
  bool Start(content::WebContents* web_contents,
             const std::string& output_path,
             int sample_rate = 48000,
             int channels = 1);

  // Stop capturing and finalize the WAV file.
  struct CaptureResult {
    double duration_ms = 0;
    int64_t samples = 0;
    double peak_rms = 0;
    std::string output_path;
  };
  CaptureResult Stop();

  // Get current audio levels.
  struct AudioLevel {
    double rms = 0;
    double peak = 0;
    bool capturing = false;
  };
  AudioLevel GetLevel() const;

  bool is_capturing() const { return capturing_; }

 private:
  bool capturing_ = false;
  std::string output_path_;
  int sample_rate_ = 48000;
  int channels_ = 1;
  int64_t total_samples_ = 0;
  double peak_rms_ = 0;
  double current_rms_ = 0;
  raw_ptr<content::WebContents> web_contents_ = nullptr;
  std::ofstream wav_file_;

  void WriteWavHeader();
  void FinalizeWavHeader();
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_ASMODEUS_AUDIO_CAPTURE_H_
