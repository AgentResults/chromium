// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/asmodeus_media_server.h"

#include <sys/stat.h>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"

namespace asmodeus {

namespace {
// Default buffer: 2 seconds of audio at given sample rate.
constexpr size_t kBufferSeconds = 2;

std::string GetMediaDir() {
  const char* home = getenv("HOME");
  return home ? std::string(home) + "/.asmodeus" : "/tmp/asmodeus-media";
}
}  // namespace

AsmodeusMediaServer::AsmodeusMediaServer() = default;

AsmodeusMediaServer::~AsmodeusMediaServer() {
  Stop();
}

bool AsmodeusMediaServer::Start(const std::string& name,
                                int sample_rate, int channels) {
  if (running_) return true;

  name_ = name;
  sample_rate_ = sample_rate;
  channels_ = channels;

  // Create the media directory.
  mkdir(GetMediaDir().c_str(), 0755);

  const size_t buffer_samples =
      static_cast<size_t>(sample_rate * channels * kBufferSeconds);

  // Create input ring buffer (external process → Chrome mic).
  // Named path: audio-in-{name}.shm
  input_path_ = GetMediaDir() + "/audio-in-" + name + ".shm";
  input_buffer_ = std::make_unique<AudioRingBuffer>();
  if (!input_buffer_->Create(input_path_, sample_rate, channels,
                             buffer_samples)) {
    LOG(ERROR) << "[Asmodeus] Failed to create input buffer: " << input_path_;
    return false;
  }

  // Create output ring buffer (Chrome audio output → external process).
  output_path_ = GetMediaDir() + "/audio-out-" + name + ".shm";
  output_buffer_ = std::make_unique<AudioRingBuffer>();
  if (!output_buffer_->Create(output_path_, sample_rate, channels,
                              buffer_samples)) {
    LOG(ERROR) << "[Asmodeus] Failed to create output buffer: " << output_path_;
    input_buffer_->Close();
    return false;
  }

  LOG(WARNING) << "[Asmodeus] MediaServer '" << name << "' started."
               << " input=" << input_path_ << " output=" << output_path_;

  running_ = true;
  return true;
}

void AsmodeusMediaServer::Stop() {
  if (!running_) return;
  running_ = false;

  LOG(WARNING) << "[Asmodeus] MediaServer '" << name_ << "' stopping.";

  if (input_buffer_) {
    input_buffer_->Close();
    unlink(input_path_.c_str());
  }
  if (output_buffer_) {
    output_buffer_->Close();
    unlink(output_path_.c_str());
  }
}

}  // namespace asmodeus
