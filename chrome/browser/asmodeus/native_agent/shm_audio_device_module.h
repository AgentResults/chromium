// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SHM_AUDIO_DEVICE_MODULE_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SHM_AUDIO_DEVICE_MODULE_H_

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "api/audio/audio_device.h"
#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"
#include "base/memory/raw_ptr.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"

namespace asmodeus {

// Custom AudioDeviceModule that reads audio from shared memory.
// Used by the native agent to feed TTS audio into WebRTC.
class ShmAudioDeviceModule : public webrtc::AudioDeviceModule {
 public:
  static webrtc::scoped_refptr<ShmAudioDeviceModule> Create(
      const std::string& capture_shm_path,
      const std::string& playout_shm_path,
      int sample_rate);

  // AudioDeviceModule implementation
  int32_t ActiveAudioLayer(AudioLayer* audio_layer) const override;
  int32_t RegisterAudioCallback(webrtc::AudioTransport* transport) override;
  int32_t Init() override;
  int32_t Terminate() override;
  bool Initialized() const override;
  int16_t PlayoutDevices() override;
  int16_t RecordingDevices() override;
  int32_t PlayoutDeviceName(uint16_t index, char name[128], char guid[128]) override;
  int32_t RecordingDeviceName(uint16_t index, char name[128], char guid[128]) override;
  int32_t SetPlayoutDevice(uint16_t index) override;
  int32_t SetPlayoutDevice(WindowsDeviceType device) override;
  int32_t SetRecordingDevice(uint16_t index) override;
  int32_t SetRecordingDevice(WindowsDeviceType device) override;
  int32_t PlayoutIsAvailable(bool* available) override;
  int32_t InitPlayout() override;
  bool PlayoutIsInitialized() const override;
  int32_t RecordingIsAvailable(bool* available) override;
  int32_t InitRecording() override;
  bool RecordingIsInitialized() const override;
  int32_t StartPlayout() override;
  int32_t StopPlayout() override;
  bool Playing() const override;
  int32_t StartRecording() override;
  int32_t StopRecording() override;
  bool Recording() const override;
  int32_t InitSpeaker() override;
  bool SpeakerIsInitialized() const override;
  int32_t InitMicrophone() override;
  bool MicrophoneIsInitialized() const override;
  int32_t SpeakerVolumeIsAvailable(bool* available) override;
  int32_t SetSpeakerVolume(uint32_t volume) override;
  int32_t SpeakerVolume(uint32_t* volume) const override;
  int32_t MaxSpeakerVolume(uint32_t* max_volume) const override;
  int32_t MinSpeakerVolume(uint32_t* min_volume) const override;
  int32_t MicrophoneVolumeIsAvailable(bool* available) override;
  int32_t SetMicrophoneVolume(uint32_t volume) override;
  int32_t MicrophoneVolume(uint32_t* volume) const override;
  int32_t MaxMicrophoneVolume(uint32_t* max_volume) const override;
  int32_t MinMicrophoneVolume(uint32_t* min_volume) const override;
  int32_t SpeakerMuteIsAvailable(bool* available) override;
  int32_t SetSpeakerMute(bool enable) override;
  int32_t SpeakerMute(bool* enabled) const override;
  int32_t MicrophoneMuteIsAvailable(bool* available) override;
  int32_t SetMicrophoneMute(bool enable) override;
  int32_t MicrophoneMute(bool* enabled) const override;
  int32_t StereoPlayoutIsAvailable(bool* available) const override;
  int32_t SetStereoPlayout(bool enable) override;
  int32_t StereoPlayout(bool* enabled) const override;
  int32_t StereoRecordingIsAvailable(bool* available) const override;
  int32_t SetStereoRecording(bool enable) override;
  int32_t StereoRecording(bool* enabled) const override;
  int32_t PlayoutDelay(uint16_t* delay_ms) const override;
  bool BuiltInAECIsAvailable() const override;
  bool BuiltInAGCIsAvailable() const override;
  bool BuiltInNSIsAvailable() const override;
  int32_t EnableBuiltInAEC(bool enable) override;
  int32_t EnableBuiltInAGC(bool enable) override;
  int32_t EnableBuiltInNS(bool enable) override;

 protected:
  ShmAudioDeviceModule(const std::string& capture_shm_path,
                       const std::string& playout_shm_path,
                       int sample_rate);
  ~ShmAudioDeviceModule() override;

 private:
  void CaptureLoop();
  void PlayoutLoop();

  AudioRingBuffer capture_ring_;
  AudioRingBuffer playout_ring_;
  std::string capture_shm_path_;
  std::string playout_shm_path_;
  int sample_rate_;
  raw_ptr<webrtc::AudioTransport> audio_transport_ = nullptr;
  std::atomic<bool> recording_{false};
  std::atomic<bool> playing_{false};
  std::atomic<bool> initialized_{false};
  std::unique_ptr<std::thread> capture_thread_;
  std::unique_ptr<std::thread> playout_thread_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_SHM_AUDIO_DEVICE_MODULE_H_
