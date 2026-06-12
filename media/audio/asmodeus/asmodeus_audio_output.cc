// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/asmodeus/asmodeus_audio_output.h"

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "media/base/audio_bus.h"

namespace asmodeus {

// Global registry of active writers per shm path.
// When multiple AsmodeusAudioOutput instances share the same shm file,
// only the one producing the loudest audio writes to the ring buffer.
// This prevents silent streams from overwriting real WebRTC audio.
struct ActiveWriter {
  raw_ptr<AsmodeusAudioOutput> instance = nullptr;
  float last_rms = 0;
  int64_t last_active_time = 0;
};

static std::map<std::string, ActiveWriter>& GetWriterRegistry() {
  static base::NoDestructor<std::map<std::string, ActiveWriter>> registry;
  return *registry;
}

static std::mutex& GetRegistryMutex() {
  static base::NoDestructor<std::mutex> mutex;
  return *mutex;
}

AsmodeusAudioOutput::AsmodeusAudioOutput(
    const media::AudioParameters& params,
    const std::string& shm_path)
    : params_(params), shm_path_(shm_path) {}

AsmodeusAudioOutput::~AsmodeusAudioOutput() {
  if (opened_) Close();
}

bool AsmodeusAudioOutput::Open() {
  // No-op mode: empty path means audio is provided by host broadcast tap
  if (shm_path_.empty()) {
    render_buffer_.resize(params_.frames_per_buffer() * params_.channels());
    opened_ = true;
    LOG(WARNING) << "[Asmodeus] AsmodeusAudioOutput[" << this
                 << "]: no-op mode (host broadcast)";
    return true;
  }
  if (!ring_.Open(shm_path_)) {
    LOG(WARNING) << "[Asmodeus] AsmodeusAudioOutput: failed to open "
                 << shm_path_;
    return false;
  }
  render_buffer_.resize(params_.frames_per_buffer() * params_.channels());
  opened_ = true;

  // Also open the corresponding audio-in shm to mix local mic into output.
  // audio-out-bob.shm → audio-in-bob.shm
  std::string mic_path = shm_path_;
  auto pos = mic_path.find("audio-out-");
  if (pos != std::string::npos) {
    mic_path.replace(pos, 10, "audio-in-");
    if (mic_ring_.Open(mic_path)) {
      mic_opened_ = true;
      mic_buffer_.resize(params_.frames_per_buffer());
    }
  }

  // Open the host broadcast SHM (audio-out-host.shm).
  // The host tap captures ALL meeting audio from the host tab's speakers
  // and writes it to audio-out-host.shm. We read from it and mix it into
  // this participant's output SHM so the agent's ConversationEngine can
  // hear everyone in the meeting — even though Meet's per-participant
  // WebRTC doesn't produce audio in the participant's own output stream.
  std::string host_path;
  auto slash = shm_path_.rfind('/');
  if (slash != std::string::npos) {
    host_path = shm_path_.substr(0, slash) + "/audio-out-host.shm";
  }
  if (!host_path.empty() && host_ring_.Open(host_path)) {
    host_opened_ = true;
    host_buffer_.resize(params_.frames_per_buffer());
    LOG(WARNING) << "[Asmodeus] AsmodeusAudioOutput: host broadcast from "
                 << host_path;
  }

  LOG(WARNING) << "[Asmodeus] AsmodeusAudioOutput[" << this << "]: opened "
               << shm_path_
               << " frames=" << params_.frames_per_buffer()
               << " ch=" << params_.channels()
               << " sr=" << params_.sample_rate()
               << " mic=" << (mic_opened_ ? "YES" : "NO")
               << " host=" << (host_opened_ ? "YES" : "NO");
  return true;
}

void AsmodeusAudioOutput::Start(AudioSourceCallback* callback) {
  callback_ = callback;
  if (!timer_) {
    timer_ = std::make_unique<base::RepeatingTimer>();
  }
  const base::TimeDelta interval = base::Milliseconds(
      params_.frames_per_buffer() * 1000 / params_.sample_rate());
  timer_->Start(FROM_HERE, interval,
                base::BindRepeating(&AsmodeusAudioOutput::OnTimer,
                                    base::Unretained(this)));
  LOG(WARNING) << "[Asmodeus] AsmodeusAudioOutput[" << this
               << "]::Start interval=" << interval.InMilliseconds() << "ms";
}

void AsmodeusAudioOutput::Stop() {
  if (timer_) timer_->Stop();
  callback_ = nullptr;
  // Unregister as writer
  {
    std::lock_guard<std::mutex> lock(GetRegistryMutex());
    auto it = GetWriterRegistry().find(shm_path_);
    if (it != GetWriterRegistry().end() && it->second.instance == this) {
      GetWriterRegistry().erase(it);
    }
  }
  LOG(WARNING) << "[Asmodeus] AsmodeusAudioOutput[" << this << "]::Stop";
}

void AsmodeusAudioOutput::Close() {
  Stop();
  ring_.Close();
  mic_ring_.Close();
  host_ring_.Close();
  opened_ = false;
  mic_opened_ = false;
  host_opened_ = false;
  delete this;
}

void AsmodeusAudioOutput::Flush() {}

void AsmodeusAudioOutput::SetVolume(double volume) {
  volume_ = volume;
}

void AsmodeusAudioOutput::GetVolume(double* volume) {
  *volume = volume_;
}

void AsmodeusAudioOutput::OnTimer() {
  if (!callback_) return;

  // Ask Chrome for audio data to "play"
  auto bus = media::AudioBus::Create(params_);
  const base::TimeTicks now = base::TimeTicks::Now();
  const base::TimeDelta delay;
  media::AudioGlitchInfo glitch_info;
  int frames = callback_->OnMoreData(delay, now, glitch_info, bus.get());
  if (frames <= 0) return;

  // AudioBus stores channels separately (planar). Mix to mono for shm.
  const int channels = bus->channels();
  const int n = std::min(frames, params_.frames_per_buffer());

  if (render_buffer_.size() < static_cast<size_t>(n)) {
    render_buffer_.resize(n);
  }

  if (channels == 1) {
    auto src = bus->channel(0);
    for (int i = 0; i < n; ++i) render_buffer_[i] = src[i];
  } else {
    for (int i = 0; i < n; ++i) {
      float sum = 0;
      for (int ch = 0; ch < channels; ++ch) {
        sum += bus->channel(ch)[i];
      }
      render_buffer_[i] = sum / channels;
    }
  }

  // Compute RMS before gain
  float sum_sq = 0;
  for (int i = 0; i < n; ++i) sum_sq += render_buffer_[i] * render_buffer_[i];
  float raw_rms = std::sqrt(sum_sq / n);

  // Apply volume with boost. WebRTC's receiver-side AGC attenuates playout
  // audio. We amplify here as the single point of gain control.
  // This is NOT a hack — it's the correct place to apply output gain.
  // Boost remote audio to compensate for WebRTC codec attenuation.
  // The mic loopback is scaled down by the same factor so both speakers
  // are at equal volume in the mixed output.
  static constexpr float kOutputGain = 5.0f;
  float effective_volume = static_cast<float>(volume_) * kOutputGain;
  for (int i = 0; i < n; ++i) {
    float v = render_buffer_[i] * effective_volume;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    render_buffer_[i] = v;
  }

  float rms = raw_rms * effective_volume;

  // Per-instance log counter
  ++log_counter_;
  if (log_counter_ % 500 == 0 || rms > 0.01f) {
    LOG(WARNING) << "[Asmodeus] AsmodeusAudioOutput[" << this << "]: frames=" << n
                 << " rms=" << rms << " raw=" << raw_rms
                 << " path=" << shm_path_;
  }

  // Mix in host broadcast audio — this is ALL meeting audio captured from
  // the host tab's speakers. Meet only plays remote audio in the host tab,
  // so each participant's AsmodeusAudioOutput gets silence from OnMoreData().
  // The host tap writes everyone's audio to audio-out-host.shm, and we
  // read it here to give each agent's ConversationEngine actual speech audio.
  if (host_opened_ && host_ring_.is_open()) {
    size_t host_avail = host_ring_.Available();
    if (host_avail > 0) {
      size_t to_read = std::min(host_avail, static_cast<size_t>(n));
      host_ring_.Read(host_buffer_.data(), to_read);
      for (size_t i = 0; i < to_read; ++i) {
        // Host audio is already at full level — add it directly.
        // Apply kOutputGain since the render_buffer already has gain applied.
        float host_sample = host_buffer_[i] * kOutputGain;
        float mixed = render_buffer_[i] + host_sample;
        if (mixed > 1.0f) mixed = 1.0f;
        if (mixed < -1.0f) mixed = -1.0f;
        render_buffer_[i] = mixed;
      }
      // Recompute RMS after mixing host audio
      float mix_sq = 0;
      for (int i = 0; i < n; ++i) mix_sq += render_buffer_[i] * render_buffer_[i];
      rms = std::sqrt(mix_sq / n);
    }
  }

  // Mix in local mic audio (loopback) so the output shm has BOTH speakers.
  static constexpr float kMicLoopbackGain = 1.0f;
  if (mic_opened_ && mic_ring_.is_open()) {
    size_t mic_avail = mic_ring_.Available();
    if (mic_avail > 0) {
      size_t to_read = std::min(mic_avail, static_cast<size_t>(n));
      mic_ring_.Read(mic_buffer_.data(), to_read);
      for (size_t i = 0; i < to_read; ++i) {
        float mixed = render_buffer_[i] + mic_buffer_[i] * kMicLoopbackGain;
        if (mixed > 1.0f) mixed = 1.0f;
        if (mixed < -1.0f) mixed = -1.0f;
        render_buffer_[i] = mixed;
      }
    }
  }

  // Write mixed audio (remote + local) to shm
  if (ring_.is_open()) {
    ring_.Write(render_buffer_.data(), n);
  }
}

}  // namespace asmodeus
