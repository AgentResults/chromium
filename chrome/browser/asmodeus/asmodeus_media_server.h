// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_ASMODEUS_MEDIA_SERVER_H_
#define CHROME_BROWSER_ASMODEUS_ASMODEUS_MEDIA_SERVER_H_

#include <memory>
#include <string>

#include "media/audio/asmodeus/audio_ring_buffer.h"

namespace asmodeus {

// Manages shared memory ring buffers for a named virtual audio device.
// Each instance creates a pair of ring buffers (input + output) with
// paths derived from the device name, e.g. ~/.asmodeus/audio-in-alice.shm.
class AsmodeusMediaServer {
 public:
  AsmodeusMediaServer();
  ~AsmodeusMediaServer();

  AsmodeusMediaServer(const AsmodeusMediaServer&) = delete;
  AsmodeusMediaServer& operator=(const AsmodeusMediaServer&) = delete;

  // Start the media server with a device name.
  // Creates shm files at ~/.asmodeus/audio-in-{name}.shm and
  // ~/.asmodeus/audio-out-{name}.shm.
  bool Start(const std::string& name, int sample_rate, int channels);

  // Stop and clean up shared memory files.
  void Stop();

  bool is_running() const { return running_; }
  const std::string& name() const { return name_; }
  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }

  // Get the ring buffers for direct access.
  AudioRingBuffer* input_buffer() { return input_buffer_.get(); }
  AudioRingBuffer* output_buffer() { return output_buffer_.get(); }

  // Paths to shared memory files (for CDP response).
  const std::string& input_path() const { return input_path_; }
  const std::string& output_path() const { return output_path_; }

  // Chrome device ID for this virtual mic (e.g. "asmodeus-alice").
  std::string device_id() const { return "asmodeus-" + name_; }

 private:
  bool running_ = false;
  std::string name_;
  int sample_rate_ = 48000;
  int channels_ = 1;
  std::string input_path_;
  std::string output_path_;
  std::unique_ptr<AudioRingBuffer> input_buffer_;
  std::unique_ptr<AudioRingBuffer> output_buffer_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_ASMODEUS_MEDIA_SERVER_H_
