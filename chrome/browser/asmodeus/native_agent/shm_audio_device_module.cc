// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/shm_audio_device_module.h"

#include <algorithm>
#include <cstring>

#include "base/logging.h"

namespace asmodeus {

webrtc::scoped_refptr<ShmAudioDeviceModule> ShmAudioDeviceModule::Create(
    const std::string& capture_shm_path,
    const std::string& playout_shm_path,
    int sample_rate) {
  return webrtc::make_ref_counted<ShmAudioDeviceModule>(
      capture_shm_path, playout_shm_path, sample_rate);
}

ShmAudioDeviceModule::ShmAudioDeviceModule(
    const std::string& capture_shm_path,
    const std::string& playout_shm_path,
    int sample_rate)
    : capture_shm_path_(capture_shm_path),
      playout_shm_path_(playout_shm_path),
      sample_rate_(sample_rate) {}

ShmAudioDeviceModule::~ShmAudioDeviceModule() {
  StopRecording();
  StopPlayout();
  capture_ring_.Close();
  playout_ring_.Close();
}

int32_t ShmAudioDeviceModule::Init() {
  if (!capture_ring_.Open(capture_shm_path_)) {
    LOG(ERROR) << "ShmADM: Failed to open capture shm: " << capture_shm_path_;
    return -1;
  }
  if (!playout_shm_path_.empty()) {
    if (!playout_ring_.Open(playout_shm_path_)) {
      LOG(ERROR) << "ShmADM: Failed to open playout shm: " << playout_shm_path_;
      return -1;
    }
  }
  initialized_ = true;
  return 0;
}

int32_t ShmAudioDeviceModule::Terminate() {
  StopRecording();
  StopPlayout();
  capture_ring_.Close();
  playout_ring_.Close();
  initialized_ = false;
  return 0;
}

int32_t ShmAudioDeviceModule::RegisterAudioCallback(
    webrtc::AudioTransport* transport) {
  audio_transport_ = transport;
  return 0;
}

int32_t ShmAudioDeviceModule::StartRecording() {
  if (recording_) return 0;
  recording_ = true;
  capture_thread_ = std::make_unique<std::thread>(&ShmAudioDeviceModule::CaptureLoop, this);
  return 0;
}

int32_t ShmAudioDeviceModule::StopRecording() {
  if (!recording_) return 0;
  recording_ = false;
  if (capture_thread_ && capture_thread_->joinable()) {
    capture_thread_->join();
  }
  capture_thread_.reset();
  return 0;
}

void ShmAudioDeviceModule::CaptureLoop() {
  const int samples_per_10ms = sample_rate_ / 100;  // 480 at 48kHz
  std::vector<int16_t> buffer(samples_per_10ms);
  std::vector<float> float_buf(samples_per_10ms);

  while (recording_) {
    // Read from capture shm ring buffer (float samples)
    size_t read = capture_ring_.Read(float_buf.data(), samples_per_10ms);

    // Convert float → int16
    for (int i = 0; i < samples_per_10ms; i++) {
      float v = (static_cast<size_t>(i) < read) ? float_buf[i] : 0.0f;
      buffer[i] = static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * 32767);
    }

    // Deliver to WebRTC
    if (audio_transport_) {
      uint32_t new_mic_level = 0;
      audio_transport_->RecordedDataIsAvailable(
          buffer.data(), samples_per_10ms,
          /*bytes_per_sample=*/2, /*channels=*/1, sample_rate_,
          /*total_delay_ms=*/0, /*clock_drift=*/0,
          /*current_mic_level=*/0, /*key_pressed=*/false,
          new_mic_level);
    }

    // Sleep ~10ms
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void ShmAudioDeviceModule::PlayoutLoop() {
  const int samples_per_10ms = sample_rate_ / 100;  // 480 at 48kHz
  const size_t n_channels = 1;
  const size_t bytes_per_sample = 2;
  std::vector<int16_t> buffer(samples_per_10ms);

  while (playing_) {
    if (audio_transport_) {
      size_t samples_out = 0;
      int64_t elapsed_time_ms = -1;
      int64_t ntp_time_ms = -1;

      // Get mixed peer audio from WebRTC.
      audio_transport_->NeedMorePlayData(
          samples_per_10ms, bytes_per_sample, n_channels, sample_rate_,
          buffer.data(), samples_out, &elapsed_time_ms, &ntp_time_ms);

      // Convert int16 → float and write to playout shm.
      if (samples_out > 0) {
        std::vector<float> float_buf(samples_out);
        for (size_t i = 0; i < samples_out; ++i) {
          float_buf[i] = buffer[i] / 32768.0f;
        }
        playout_ring_.Write(float_buf.data(), samples_out);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// === No-op implementations for unused ADM methods ===

int32_t ShmAudioDeviceModule::ActiveAudioLayer(AudioLayer* al) const { return 0; }
bool ShmAudioDeviceModule::Initialized() const { return initialized_; }
int16_t ShmAudioDeviceModule::PlayoutDevices() { return 0; }
int16_t ShmAudioDeviceModule::RecordingDevices() { return 1; }
int32_t ShmAudioDeviceModule::PlayoutDeviceName(uint16_t, char n[128], char g[128]) { n[0]=0; g[0]=0; return 0; }
int32_t ShmAudioDeviceModule::RecordingDeviceName(uint16_t, char n[128], char g[128]) { strncpy(n,"ShmMic",128); g[0]=0; return 0; }
int32_t ShmAudioDeviceModule::SetPlayoutDevice(uint16_t) { return 0; }
int32_t ShmAudioDeviceModule::SetPlayoutDevice(WindowsDeviceType) { return 0; }
int32_t ShmAudioDeviceModule::SetRecordingDevice(uint16_t) { return 0; }
int32_t ShmAudioDeviceModule::SetRecordingDevice(WindowsDeviceType) { return 0; }
int32_t ShmAudioDeviceModule::PlayoutIsAvailable(bool* a) { *a=!playout_shm_path_.empty(); return 0; }
int32_t ShmAudioDeviceModule::InitPlayout() { return 0; }
bool ShmAudioDeviceModule::PlayoutIsInitialized() const { return !playout_shm_path_.empty(); }
int32_t ShmAudioDeviceModule::RecordingIsAvailable(bool* a) { *a=true; return 0; }
int32_t ShmAudioDeviceModule::InitRecording() { return 0; }
bool ShmAudioDeviceModule::RecordingIsInitialized() const { return true; }
int32_t ShmAudioDeviceModule::StartPlayout() {
  if (playing_ || playout_shm_path_.empty()) return 0;
  playing_ = true;
  playout_thread_ = std::make_unique<std::thread>(
      &ShmAudioDeviceModule::PlayoutLoop, this);
  return 0;
}

int32_t ShmAudioDeviceModule::StopPlayout() {
  if (!playing_) return 0;
  playing_ = false;
  if (playout_thread_ && playout_thread_->joinable()) {
    playout_thread_->join();
  }
  playout_thread_.reset();
  return 0;
}

bool ShmAudioDeviceModule::Playing() const { return playing_; }
bool ShmAudioDeviceModule::Recording() const { return recording_; }
int32_t ShmAudioDeviceModule::InitSpeaker() { return 0; }
bool ShmAudioDeviceModule::SpeakerIsInitialized() const { return false; }
int32_t ShmAudioDeviceModule::InitMicrophone() { return 0; }
bool ShmAudioDeviceModule::MicrophoneIsInitialized() const { return true; }
int32_t ShmAudioDeviceModule::SpeakerVolumeIsAvailable(bool* a) { *a=false; return 0; }
int32_t ShmAudioDeviceModule::SetSpeakerVolume(uint32_t) { return 0; }
int32_t ShmAudioDeviceModule::SpeakerVolume(uint32_t* v) const { *v=0; return 0; }
int32_t ShmAudioDeviceModule::MaxSpeakerVolume(uint32_t* v) const { *v=0; return 0; }
int32_t ShmAudioDeviceModule::MinSpeakerVolume(uint32_t* v) const { *v=0; return 0; }
int32_t ShmAudioDeviceModule::MicrophoneVolumeIsAvailable(bool* a) { *a=false; return 0; }
int32_t ShmAudioDeviceModule::SetMicrophoneVolume(uint32_t) { return 0; }
int32_t ShmAudioDeviceModule::MicrophoneVolume(uint32_t* v) const { *v=0; return 0; }
int32_t ShmAudioDeviceModule::MaxMicrophoneVolume(uint32_t* v) const { *v=0; return 0; }
int32_t ShmAudioDeviceModule::MinMicrophoneVolume(uint32_t* v) const { *v=0; return 0; }
int32_t ShmAudioDeviceModule::SpeakerMuteIsAvailable(bool* a) { *a=false; return 0; }
int32_t ShmAudioDeviceModule::SetSpeakerMute(bool) { return 0; }
int32_t ShmAudioDeviceModule::SpeakerMute(bool* e) const { *e=false; return 0; }
int32_t ShmAudioDeviceModule::MicrophoneMuteIsAvailable(bool* a) { *a=false; return 0; }
int32_t ShmAudioDeviceModule::SetMicrophoneMute(bool) { return 0; }
int32_t ShmAudioDeviceModule::MicrophoneMute(bool* e) const { *e=false; return 0; }
int32_t ShmAudioDeviceModule::StereoPlayoutIsAvailable(bool* a) const { *a=false; return 0; }
int32_t ShmAudioDeviceModule::SetStereoPlayout(bool) { return 0; }
int32_t ShmAudioDeviceModule::StereoPlayout(bool* e) const { *e=false; return 0; }
int32_t ShmAudioDeviceModule::StereoRecordingIsAvailable(bool* a) const { *a=false; return 0; }
int32_t ShmAudioDeviceModule::SetStereoRecording(bool) { return 0; }
int32_t ShmAudioDeviceModule::StereoRecording(bool* e) const { *e=false; return 0; }
int32_t ShmAudioDeviceModule::PlayoutDelay(uint16_t* d) const { *d=0; return 0; }
bool ShmAudioDeviceModule::BuiltInAECIsAvailable() const { return false; }
bool ShmAudioDeviceModule::BuiltInAGCIsAvailable() const { return false; }
bool ShmAudioDeviceModule::BuiltInNSIsAvailable() const { return false; }
int32_t ShmAudioDeviceModule::EnableBuiltInAEC(bool) { return -1; }
int32_t ShmAudioDeviceModule::EnableBuiltInAGC(bool) { return -1; }
int32_t ShmAudioDeviceModule::EnableBuiltInNS(bool) { return -1; }

}  // namespace asmodeus
