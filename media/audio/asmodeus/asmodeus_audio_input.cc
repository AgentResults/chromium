// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/asmodeus/asmodeus_audio_input.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/threading/thread.h"
#include "base/timer/timer.h"
#include "media/base/audio_bus.h"

namespace asmodeus {

AsmodeusAudioInput::AsmodeusAudioInput(AudioRingBuffer* ring_buffer,
                                       const media::AudioParameters& params)
    : ring_buffer_(ring_buffer), params_(params) {}

AsmodeusAudioInput::~AsmodeusAudioInput() {
  Stop();
  // owned_ring_buffer_ is automatically cleaned up by unique_ptr
}

void AsmodeusAudioInput::TakeOwnership(AudioRingBuffer* ring_buffer) {
  owned_ring_buffer_.reset(ring_buffer);
}

media::AudioInputStream::OpenOutcome AsmodeusAudioInput::Open() {
  if (!ring_buffer_ || !ring_buffer_->is_open()) {
    LOG(ERROR) << "[Asmodeus] AsmodeusAudioInput::Open failed - no ring buffer";
    return OpenOutcome::kFailed;
  }
  audio_bus_ = media::AudioBus::Create(params_);
  opened_ = true;
  LOG(WARNING) << "[Asmodeus] AsmodeusAudioInput::Open success, frames="
               << params_.frames_per_buffer()
               << " sr=" << params_.sample_rate()
               << " shm_sr=" << ring_buffer_->sample_rate()
               << " ch=" << params_.channels();
  return OpenOutcome::kSuccess;
}

void AsmodeusAudioInput::Start(AudioInputCallback* callback) {
  if (!opened_ || !callback) return;
  callback_ = callback;

  // Use a dedicated real-time audio thread with a timer, same pattern as
  // Chrome's FakeAudioInputStream. The kRealtimeAudio thread priority
  // ensures the timer fires at consistent intervals.
  capture_thread_ = std::make_unique<base::Thread>("AsmodeusAudioInput");
  CHECK(capture_thread_->StartWithOptions(
      base::Thread::Options(base::ThreadType::kRealtimeAudio)));

  const base::TimeDelta interval = params_.GetBufferDuration();
  capture_thread_->task_runner()->PostTask(FROM_HERE,
      base::BindOnce([](AsmodeusAudioInput* self, base::TimeDelta interval) {
        self->timer_ = std::make_unique<base::RepeatingTimer>();
        self->timer_->Start(FROM_HERE, interval,
            base::BindRepeating(&AsmodeusAudioInput::OnTimer,
                                base::Unretained(self)));
      }, base::Unretained(this), interval));

  LOG(WARNING) << "[Asmodeus] AsmodeusAudioInput::Start on RT thread, interval="
               << interval.InMilliseconds() << "ms";
}

void AsmodeusAudioInput::Stop() {
  if (capture_thread_) {
    capture_thread_->task_runner()->PostTask(FROM_HERE,
        base::BindOnce([](AsmodeusAudioInput* self) {
          if (self->timer_) {
            self->timer_->Stop();
            self->timer_.reset();
          }
        }, base::Unretained(this)));
    capture_thread_->Stop();
    capture_thread_.reset();
  }
  callback_ = nullptr;
}

void AsmodeusAudioInput::Close() {
  Stop();
  opened_ = false;
  audio_bus_.reset();
}

void AsmodeusAudioInput::OnTimer() {
  if (!callback_ || !ring_buffer_ || !audio_bus_) return;

  const int frames = audio_bus_->frames();
  const int channels = audio_bus_->channels();
  const int chrome_sr = params_.sample_rate();
  const int shm_sr = ring_buffer_->sample_rate();

  // Calculate how many shm samples we need
  size_t shm_samples_needed = frames;
  if (shm_sr > 0 && chrome_sr > 0 && shm_sr != chrome_sr) {
    shm_samples_needed = (static_cast<size_t>(frames) * shm_sr) / chrome_sr;
  }

  // Use per-instance read position (ReadAt) instead of the shared read_pos.
  // Chrome creates multiple AsmodeusAudioInput instances that all read from
  // the same SHM. If they share read_pos, they race and one always gets 0
  // samples — causing Meet to mute the WebRTC audio track.
  std::vector<float> shm_samples(shm_samples_needed);
  size_t read = ring_buffer_->ReadAt(shm_samples.data(), shm_samples.size(),
                                      &local_read_pos_);

  // Compute raw SHM RMS before any processing
  float shm_rms = 0;
  for (size_t i = 0; i < read; ++i) shm_rms += shm_samples[i] * shm_samples[i];
  if (read > 0) shm_rms = std::sqrt(shm_rms / static_cast<float>(read));

  if (read == 0) {
    // Deliver comfort noise instead of pure silence.
    // Pure silence causes Chrome to set muted=true on the MediaStreamTrack,
    // which makes WebRTC stop encoding and transmitting audio entirely.
    // Low-level noise (~-60dBFS) keeps the track alive without being audible.
    for (int ch = 0; ch < channels; ++ch) {
      auto chan = audio_bus_->channel(ch);
      for (int f = 0; f < frames; ++f) {
        chan[static_cast<size_t>(f)] = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 0.002f;
      }
    }
  } else {
    // No resampling needed when rates match (common case)
    for (int ch = 0; ch < channels; ++ch) {
      auto chan = audio_bus_->channel(ch);
      if (shm_sr == chrome_sr || shm_sr == 0) {
        for (int f = 0; f < frames && static_cast<size_t>(f) < read; ++f) {
          chan[static_cast<size_t>(f)] = shm_samples[static_cast<size_t>(f)];
        }
      } else {
        const double ratio = static_cast<double>(shm_sr) / chrome_sr;
        for (int f = 0; f < frames; ++f) {
          double src_pos = f * ratio;
          size_t i0 = static_cast<size_t>(src_pos);
          if (i0 >= read) break;
          size_t i1 = i0 + 1;
          double frac = src_pos - i0;
          float s0 = shm_samples[i0];
          float s1 = (i1 < read) ? shm_samples[i1] : s0;
          chan[static_cast<size_t>(f)] =
              static_cast<float>(s0 * (1.0 - frac) + s1 * frac);
        }
      }
    }
  }

  // Log audio delivery stats
  ++total_frames_;
  if (read > 0) ++nonzero_frames_;

  float rms = 0;
  auto chan0 = audio_bus_->channel(0);
  for (int f = 0; f < frames; ++f) rms += chan0[f] * chan0[f];
  rms = std::sqrt(rms / frames);

  // Log every 500 frames (~5s) with cumulative stats, or on transitions
  bool was_active = prev_input_rms_ > 0.003f;
  bool now_active = rms > 0.003f;
  prev_input_rms_ = rms;

  if (total_frames_ <= 3 || total_frames_ % 500 == 0 ||
      (was_active != now_active)) {
    LOG(WARNING) << "[Asmodeus] AudioInput[" << this
                 << "] shm_rms=" << shm_rms << " out_rms=" << rms
                 << " read=" << read
                 << " nonzero=" << nonzero_frames_
                 << "/" << total_frames_
                 << " rp=" << local_read_pos_
                 << (was_active != now_active
                     ? (now_active ? " ▶AUDIO" : " ■SILENT") : "");
  }

  callback_->OnData(audio_bus_.get(), base::TimeTicks::Now(), volume_, {});
}

double AsmodeusAudioInput::GetMaxVolume() { return 1.0; }
void AsmodeusAudioInput::SetVolume(double volume) { volume_ = volume; }
double AsmodeusAudioInput::GetVolume() { return volume_; }
bool AsmodeusAudioInput::IsMuted() { return false; }
void AsmodeusAudioInput::SetOutputDeviceForAec(
    const std::string& output_device_id) {}
bool AsmodeusAudioInput::SetAutomaticGainControl(bool enabled) { return false; }
bool AsmodeusAudioInput::GetAutomaticGainControl() { return false; }

}  // namespace asmodeus
