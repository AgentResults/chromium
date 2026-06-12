// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/asmodeus/asmodeus_audio_output_tap.h"

#include <cmath>
#include <dirent.h>
#include <map>
#include <mutex>
#include <sys/stat.h>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "media/base/audio_bus.h"
#include "media/base/media_switches.h"

namespace asmodeus {

// Global persistent broadcast buffers. Survives stream restarts so that
// write positions are monotonic and agents always see continuous audio.
// Meet creates short-lived audio output streams (2-3s each). Without
// persistent buffers, each new stream would reset the write position
// and agents would miss audio during the gap between streams.
struct PersistentBroadcast {
  AudioRingBuffer host_buffer;
  std::map<std::string, AudioRingBuffer*> participant_buffers;
  std::mutex mu;
  bool initialized = false;
  int scan_counter = 0;

  void EnsureInitialized(const std::string& host_shm_path) {
    std::lock_guard<std::mutex> lock(mu);
    if (initialized) return;
    if (host_buffer.CreateIfNeeded(host_shm_path, 48000, 1, 48000 * 10)) {
      initialized = true;
      LOG(WARNING) << "[Asmodeus] PersistentBroadcast: host buffer "
                   << host_shm_path;
    }
  }

  void ScanParticipants() {
    std::lock_guard<std::mutex> lock(mu);
    const char* home = getenv("HOME");
    if (!home) return;
    std::string dir = std::string(home) + "/.asmodeus";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
      std::string fn(entry->d_name);
      if (fn.size() > 14 &&
          fn.substr(0, 10) == "audio-out-" &&
          fn.substr(fn.size() - 4) == ".shm" &&
          fn != "audio-out-host.shm") {
        if (participant_buffers.count(fn)) continue;  // Already open
        std::string path = dir + "/" + fn;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && st.st_size > 16) {
          auto* buf = new AudioRingBuffer();
          if (buf->Open(path)) {
            participant_buffers[fn] = buf;
            LOG(WARNING) << "[Asmodeus] PersistentBroadcast: added " << fn;
          } else {
            delete buf;
          }
        }
      }
    }
    closedir(d);
  }

  void Broadcast(const float* mono, size_t count,
                 const std::string& only_name) {
    std::lock_guard<std::mutex> lock(mu);
    if (initialized) {
      host_buffer.Write(mono, count);
    }
    // Write ONLY to our own agent's audio-out SHM.
    // Meet already handles audio routing: each Chrome only receives
    // OTHER participants' audio. So the OutputTap captures the mixed
    // audio of all remote participants and writes it to THIS agent's
    // listening SHM. Broadcasting to OTHER agents' SHMs would cause
    // them to hear their own echo (since Meet routes their mic audio
    // to this Chrome's speakers).
    if (!only_name.empty()) {
      auto it = participant_buffers.find(only_name);
      if (it != participant_buffers.end() && it->second->is_open()) {
        it->second->Write(mono, count);
      }
    } else {
      // No filter — write to all (host Chrome without --asmodeus-device)
      for (auto& [name, buf] : participant_buffers) {
        if (buf->is_open()) {
          buf->Write(mono, count);
        }
      }
    }
  }
};

static PersistentBroadcast& GetBroadcast() {
  static base::NoDestructor<PersistentBroadcast> instance;
  return *instance;
}

// Called from AudioManagerMac to initialize the persistent broadcast.
void InitPersistentBroadcast(const std::string& host_shm_path) {
  GetBroadcast().EnsureInitialized(host_shm_path);
}

// ──────────────────────────────────────────────────────────

AsmodeusAudioOutputTap::AsmodeusAudioOutputTap(
    media::AudioOutputStream* wrapped,
    AudioRingBuffer* output_buffer)
    : wrapped_(wrapped), output_buffer_(output_buffer),
      owns_buffer_(false), gain_(1.0f) {
}

AsmodeusAudioOutputTap::AsmodeusAudioOutputTap(
    media::AudioOutputStream* wrapped,
    AudioRingBuffer* output_buffer,
    bool owns_buffer,
    float gain)
    : wrapped_(wrapped), output_buffer_(output_buffer),
      owns_buffer_(owns_buffer), gain_(gain) {
}

AsmodeusAudioOutputTap::~AsmodeusAudioOutputTap() {
  // Don't close participant buffers — they're owned by PersistentBroadcast.
  // Only close the per-tap output buffer if we own it.
  if (owns_buffer_ && output_buffer_) {
    output_buffer_->Close();
    delete output_buffer_;
    output_buffer_ = nullptr;
  }
}

void AsmodeusAudioOutputTap::ScanParticipantBuffers() {
  // Delegate to the global persistent broadcast
  GetBroadcast().ScanParticipants();
}

bool AsmodeusAudioOutputTap::Open() {
  return wrapped_->Open();
}

void AsmodeusAudioOutputTap::Start(AudioSourceCallback* callback) {
  real_callback_ = callback;
  is_active_ = true;

  // Mute real speakers for agent Chrome instances.
  // When --asmodeus-device is set, this Chrome is an agent — it should
  // NOT play audio through real speakers (causes feedback loops).
  // The tap still captures the audio data for writing to SHM.
  auto* cmd = base::CommandLine::ForCurrentProcess();
  bool mute_speakers = owns_buffer_ ||
      cmd->HasSwitch(switches::kAsmodeusDevice);

  // Cache the agent name for Broadcast — write ONLY to our own SHM.
  // Meet routes other participants' audio to our speakers, so we
  // capture it and write to our agent's listening SHM.
  if (cmd->HasSwitch(switches::kAsmodeusDevice)) {
    skip_name_ = "audio-out-" +
        cmd->GetSwitchValueASCII(switches::kAsmodeusDevice) + ".shm";
  }

  LOG(WARNING) << "[Asmodeus] OutputTap[" << this
               << "]::Start gain=" << gain_
               << " mute=" << (mute_speakers ? "YES" : "NO")
               << " only=" << skip_name_;
  wrapped_->Start(this);
  if (mute_speakers) {
    wrapped_->SetVolume(0.0);
  }
}

void AsmodeusAudioOutputTap::Stop() {
  LOG(WARNING) << "[Asmodeus] OutputTap[" << this << "]::Stop"
               << " total=" << total_calls_
               << " nonzero=" << nonzero_calls_;
  is_active_ = false;
  wrapped_->Stop();
  real_callback_ = nullptr;
}

void AsmodeusAudioOutputTap::Close() {
  wrapped_->Close();
}

void AsmodeusAudioOutputTap::Flush() {
  wrapped_->Flush();
}

void AsmodeusAudioOutputTap::SetVolume(double volume) {
  if (owns_buffer_) {
    wrapped_->SetVolume(0.0);
  } else {
    wrapped_->SetVolume(volume);
  }
}

void AsmodeusAudioOutputTap::GetVolume(double* volume) {
  wrapped_->GetVolume(volume);
}

int AsmodeusAudioOutputTap::OnMoreData(
    base::TimeDelta delay,
    base::TimeTicks delay_timestamp,
    const media::AudioGlitchInfo& glitch_info,
    media::AudioBus* dest) {
  int frames = 0;
  if (real_callback_) {
    frames = real_callback_->OnMoreData(delay, delay_timestamp,
                                        glitch_info, dest);
  }
  if (frames <= 0 || !dest) return frames;

  ++total_calls_;

  // Mix to mono
  int channels = dest->channels();
  std::vector<float> mono(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    float sample = 0;
    for (int ch = 0; ch < channels; ++ch) {
      sample += dest->channel(ch)[i];
    }
    sample = (sample / channels) * gain_;
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    mono[static_cast<size_t>(i)] = sample;
  }

  // Compute RMS
  float sum_sq = 0;
  for (int i = 0; i < frames; i++) sum_sq += mono[i] * mono[i];
  float rms = std::sqrt(sum_sq / frames);

  // Periodically scan for new participants (every ~5 seconds at 10ms intervals)
  auto& broadcast = GetBroadcast();
  if (total_calls_ % 500 == 1) {
    broadcast.ScanParticipants();
  }

  // Broadcast to participant SHMs (skipping our own agent's SHM)
  broadcast.Broadcast(mono.data(), static_cast<size_t>(frames), skip_name_);

  if (rms > 0.003f) {
    ++nonzero_calls_;
    is_active_ = true;
  }

  // Log every 500 frames (~5s) with summary stats, or when audio transitions
  bool was_active = prev_rms_ > 0.003f;
  bool now_active = rms > 0.003f;
  bool transition = (was_active != now_active);
  prev_rms_ = rms;

  if (total_calls_ <= 3 || total_calls_ % 500 == 0 || transition) {
    LOG(WARNING) << "[Asmodeus] OutputTap[" << this << "] frames=" << frames
                 << " rms=" << rms << " gain=" << gain_
                 << " nonzero=" << nonzero_calls_
                 << "/" << total_calls_
                 << " only=" << skip_name_
                 << (transition ? (now_active ? " ▶AUDIO" : " ■SILENT") : "");
  }

  return frames;
}

void AsmodeusAudioOutputTap::OnError(ErrorType type) {
  if (real_callback_) {
    real_callback_->OnError(type);
  }
}

}  // namespace asmodeus
