// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/speech_output.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "base/logging.h"
#include "chrome/browser/asmodeus/native_agent/avatar_renderer.h"
#include "chrome/browser/asmodeus/native_agent/video_shm.h"
#include "third_party/libyuv/include/libyuv.h"

namespace asmodeus {

SpeechOutput::Config::Config() = default;
SpeechOutput::Config::~Config() = default;
SpeechOutput::Config::Config(const Config&) = default;
SpeechOutput::Config& SpeechOutput::Config::operator=(const Config&) = default;

SpeechOutput::SpeechOutput(Config config,
                           AudioRingBuffer* audio_in_shm,
                           AvatarRenderer* avatar_renderer,
                           void* video_shm_mapped,
                           OnAecReference on_aec_ref,
                           OnSpeechStarted on_started,
                           OnSpeechEnded on_ended)
    : config_(std::move(config)),
      on_aec_ref_(std::move(on_aec_ref)),
      on_started_(std::move(on_started)),
      on_ended_(std::move(on_ended)),
      audio_in_shm_(audio_in_shm),
      avatar_renderer_(avatar_renderer),
      video_shm_mapped_(video_shm_mapped) {}

SpeechOutput::~SpeechOutput() = default;

void SpeechOutput::Cancel() {
  cancel_.store(true);
}

void SpeechOutput::Speak(const std::string& text) {
  if (text.empty()) return;
  cancel_.store(false);
  speaking_.store(true);

  if (on_started_) on_started_(text);
  auto t0 = std::chrono::steady_clock::now();

  // Spawn Piper TTS process.
  int stdin_pipe[2], stdout_pipe[2];
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
    LOG(ERROR) << "Failed to create pipes for TTS";
    speaking_.store(false);
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    LOG(ERROR) << "Fork failed for TTS";
    close(stdin_pipe[0]); close(stdin_pipe[1]);
    close(stdout_pipe[0]); close(stdout_pipe[1]);
    speaking_.store(false);
    return;
  }

  if (pid == 0) {
    close(stdin_pipe[1]); close(stdout_pipe[0]);
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[0]); close(stdout_pipe[1]);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
    std::string scale = std::to_string(config_.length_scale);
    execl(config_.piper_path.c_str(), "piper",
          "--model", config_.voice_model.c_str(),
          "--length-scale", scale.c_str(),
          "--output_raw", "--output_file", "-", nullptr);
    _exit(1);
  }

  close(stdin_pipe[0]); close(stdout_pipe[1]);
  std::string input = text + "\n";
  write(stdin_pipe[1], input.c_str(), input.size());
  close(stdin_pipe[1]);

  const int chunk_samples = 441;  // 20ms at 22050Hz
  int16_t pcm_buf[441];

  while (!cancel_.load()) {
    ssize_t bytes = read(stdout_pipe[0], pcm_buf,
                         chunk_samples * sizeof(int16_t));
    if (bytes <= 0) break;
    int samples = static_cast<int>(bytes / 2);

    // Resample 22050 → 48000 and write to audio-in shm.
    const float ratio = 22050.0f / 48000.0f;
    const float gain = 2.5f;
    int out_n = static_cast<int>(samples / ratio);
    std::vector<float> resampled(out_n);
    for (int i = 0; i < out_n; ++i) {
      float src_pos = i * ratio;
      int idx = static_cast<int>(src_pos);
      float frac = src_pos - idx;
      float a = (idx < samples) ? pcm_buf[idx] / 32768.0f : 0.0f;
      float b = (idx + 1 < samples) ? pcm_buf[idx + 1] / 32768.0f : a;
      float v = (a + (b - a) * frac) * gain;
      if (v > 1.0f) v = 1.0f;
      if (v < -1.0f) v = -1.0f;
      resampled[i] = v;
    }

    if (audio_in_shm_) {
      audio_in_shm_->Write(resampled.data(), resampled.size());
    }

    // Compute RMS and render avatar.
    float rms = 0.0f;
    for (float s : resampled) rms += s * s;
    rms = std::sqrt(rms / static_cast<float>(resampled.size()));

    if (avatar_renderer_ && video_shm_mapped_) {
      float jaw_open = std::clamp(rms * 15.0f, 0.0f, 1.0f);
      SkBitmap frame = avatar_renderer_->Render(jaw_open);
      auto* vh = reinterpret_cast<VideoShmHeader*>(video_shm_mapped_);
      uint32_t fs = vh->frame_size;
      uint32_t wb = 1 - vh->current_buffer.load();
      uint8_t* base_ptr = reinterpret_cast<uint8_t*>(video_shm_mapped_) +
                          sizeof(VideoShmHeader);
      uint8_t* nv12 = base_ptr + wb * fs;
      int w = static_cast<int>(vh->width);
      int h = static_cast<int>(vh->height);
      libyuv::ARGBToNV12(
          reinterpret_cast<const uint8_t*>(frame.getPixels()), w * 4,
          nv12, w, nv12 + w * h, w, w, h);
      vh->frame_sequence.fetch_add(1);
      vh->current_buffer.store(wb);
    }

    // Feed AEC reference (resample 22050 → 16000).
    if (on_aec_ref_) {
      const float ratio16 = 22050.0f / 16000.0f;
      int out16_n = static_cast<int>(samples / ratio16);
      std::vector<float> ref16(out16_n);
      for (int i = 0; i < out16_n; ++i) {
        float src_pos = i * ratio16;
        int idx = static_cast<int>(src_pos);
        float frac = src_pos - idx;
        float a = (idx < samples) ? pcm_buf[idx] / 32768.0f : 0.0f;
        float b = (idx + 1 < samples) ? pcm_buf[idx + 1] / 32768.0f : a;
        float v = (a + (b - a) * frac) * gain;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        ref16[i] = v;
      }
      on_aec_ref_(ref16.data(), out16_n);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  close(stdout_pipe[0]);
  int status = 0;
  waitpid(pid, &status, 0);

  speaking_.store(false);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  if (on_ended_) on_ended_(text, static_cast<double>(ms));
}

}  // namespace asmodeus
