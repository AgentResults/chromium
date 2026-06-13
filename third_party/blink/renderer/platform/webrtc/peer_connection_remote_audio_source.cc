// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/webrtc/peer_connection_remote_audio_source.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <utility>

#include "base/check_op.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/strings/to_string.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_glitch_info.h"
#include "media/base/audio_sample_types.h"
#include "third_party/blink/public/platform/modules/webrtc/webrtc_logging.h"

namespace blink {

BASE_FEATURE(kPropagateEnabledEventForWebRtcAudioTrack,
             base::FEATURE_ENABLED_BY_DEFAULT);

namespace {
// Used as an identifier for the down-casters.
void* const kPeerConnectionRemoteTrackIdentifier =
    const_cast<void**>(&kPeerConnectionRemoteTrackIdentifier);

void SendLogMessage(const std::string& message) {
  blink::WebRtcLogMessage("PCRAS::" + message);
}

}  // namespace

PeerConnectionRemoteAudioTrack::PeerConnectionRemoteAudioTrack(
    scoped_refptr<webrtc::AudioTrackInterface> track_interface)
    : MediaStreamAudioTrack(false /* is_local_track */),
      track_interface_(std::move(track_interface)) {
  blink::WebRtcLogMessage(
      base::StringPrintf("PCRAT::PeerConnectionRemoteAudioTrack({id=%s})",
                         track_interface_->id().c_str()));
}

PeerConnectionRemoteAudioTrack::~PeerConnectionRemoteAudioTrack() {
  blink::WebRtcLogMessage(
      base::StringPrintf("PCRAT::~PeerConnectionRemoteAudioTrack([id=%s])",
                         track_interface_->id().c_str()));
  // Ensure the track is stopped.
  MediaStreamAudioTrack::Stop();
}

// static
PeerConnectionRemoteAudioTrack* PeerConnectionRemoteAudioTrack::From(
    MediaStreamAudioTrack* track) {
  if (track &&
      track->GetClassIdentifier() == kPeerConnectionRemoteTrackIdentifier)
    return static_cast<PeerConnectionRemoteAudioTrack*>(track);
  return nullptr;
}

void PeerConnectionRemoteAudioTrack::SetEnabled(bool enabled) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  blink::WebRtcLogMessage(base::StringPrintf(
      "PCRAT::SetEnabled([id=%s] {enabled=%s})", track_interface_->id().c_str(),
      base::ToString(enabled).c_str()));

  if (!base::FeatureList::IsEnabled(
          kPropagateEnabledEventForWebRtcAudioTrack)) {
    // This affects the shared state of the source for whether or not it's a
    // part of the mixed audio that's rendered for remote tracks from WebRTC.
    // All tracks from the same source will share this state and thus can step
    // on each other's toes.
    // This is also why we can't check the enabled state for equality with
    // |enabled| before setting the mixing enabled state. This track's enabled
    // state and the shared state might not be the same.
    track_interface_->set_enabled(enabled);
  }

  MediaStreamAudioTrack::SetEnabled(enabled);
}

void* PeerConnectionRemoteAudioTrack::GetClassIdentifier() const {
  return kPeerConnectionRemoteTrackIdentifier;
}

PeerConnectionRemoteAudioSource::PeerConnectionRemoteAudioSource(
    scoped_refptr<webrtc::AudioTrackInterface> track_interface,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    : MediaStreamAudioSource(std::move(task_runner),
                             false /* is_local_source */),
      track_interface_(std::move(track_interface)),
      is_sink_of_peer_connection_(false) {
  DCHECK(track_interface_);
  SendLogMessage(base::StringPrintf("PeerConnectionRemoteAudioSource([id=%s])",
                                    track_interface_->id().c_str()));
}

PeerConnectionRemoteAudioSource::~PeerConnectionRemoteAudioSource() {
  SendLogMessage(base::StringPrintf("~PeerConnectionRemoteAudioSource([id=%s])",
                                    track_interface_->id().c_str()));
  EnsureSourceIsStopped();
  // AURELIAN-MEDIA-CONTROL §5.2: release the ear-ring mapping.
  if (asmodeus_ear_map_) {
    munmap(asmodeus_ear_map_, asmodeus_ear_map_size_);
    asmodeus_ear_map_ = nullptr;
  }
  if (asmodeus_ear_fd_ >= 0) {
    close(asmodeus_ear_fd_);
    asmodeus_ear_fd_ = -1;
  }
}

std::unique_ptr<MediaStreamAudioTrack>
PeerConnectionRemoteAudioSource::CreateMediaStreamAudioTrack(
    const std::string& id) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return std::make_unique<PeerConnectionRemoteAudioTrack>(track_interface_);
}

bool PeerConnectionRemoteAudioSource::EnsureSourceIsStarted() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (is_sink_of_peer_connection_)
    return true;
  SendLogMessage(base::StringPrintf("EnsureSourceIsStarted([id=%s])",
                                    track_interface_->id().c_str()));
  track_interface_->AddSink(this);
  is_sink_of_peer_connection_ = true;
  return true;
}

void PeerConnectionRemoteAudioSource::EnsureSourceIsStopped() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  if (is_sink_of_peer_connection_) {
    SendLogMessage(base::StringPrintf("EnsureSourceIsStopped([id=%s])",
                                      track_interface_->id().c_str()));
    track_interface_->RemoveSink(this);
    is_sink_of_peer_connection_ = false;
  }
}

void PeerConnectionRemoteAudioSource::OnData(const void* audio_data,
                                             int bits_per_sample,
                                             int sample_rate,
                                             size_t number_of_channels,
                                             size_t number_of_frames) {
  // Debug builds: Note that this lock isn't meant to synchronize anything.
  // Instead, it is being used as a run-time check to ensure there isn't already
  // another thread executing this method. The reason we don't use
  // base::ThreadChecker here is because we shouldn't be making assumptions
  // about the private threading model of libjingle. For example, it would be
  // legitimate for libjingle to use a different thread to invoke this method
  // whenever the audio format changes.
#ifndef NDEBUG
  CHECK(single_audio_thread_guard_.Try());
#endif

  TRACE_EVENT2("audio", "PeerConnectionRemoteAudioSource::OnData",
               "sample_rate", sample_rate, "number_of_frames",
               number_of_frames);
  // TODO(tommi): We should get the timestamp from WebRTC.
  base::TimeTicks playout_time(base::TimeTicks::Now());

  int channels_int = base::checked_cast<int>(number_of_channels);
  int frames_int = base::checked_cast<int>(number_of_frames);
  if (!audio_bus_ || audio_bus_->channels() != channels_int ||
      audio_bus_->frames() != frames_int) {
    audio_bus_ = media::AudioBus::Create(channels_int, frames_int);
  }

  // Only 16 bits per sample is ever used. The FromInterleaved() call should
  // be updated if that is no longer the case.
  CHECK_EQ(bits_per_sample, 16);

  size_t total_samples =
      base::CheckMul(number_of_channels, number_of_frames).ValueOrDie();

  // SAFETY: Per interface contract, `data` should contain `number_of_frames` *
  // `number_of_channels` samples, each sample being `sizeof(int16_t)` wide.
  auto source = UNSAFE_BUFFERS(
      base::span(reinterpret_cast<const int16_t*>(audio_data), total_samples));
  audio_bus_->FromInterleaved<media::SignedInt16SampleTypeTraits>(source);

  media::AudioParameters params = MediaStreamAudioSource::GetAudioParameters();
  if (!params.IsValid() ||
      params.format() != media::AudioParameters::AUDIO_PCM_LOW_LATENCY ||
      params.channels() != channels_int ||
      params.sample_rate() != sample_rate ||
      params.frames_per_buffer() != frames_int) {
    MediaStreamAudioSource::SetFormat(
        media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                               media::ChannelLayoutConfig::Guess(channels_int),
                               sample_rate, frames_int));
  }

  MediaStreamAudioSource::DeliverDataToTracks(*audio_bus_, playout_time, {});

  // AURELIAN-MEDIA-CONTROL §5.2: capture the decoded remote PCM at the engine.
  AsmodeusWriteEar(*audio_bus_, sample_rate);

#ifndef NDEBUG
  single_audio_thread_guard_.Release();
#endif
}

void PeerConnectionRemoteAudioSource::AsmodeusWriteEar(const media::AudioBus& bus,
                                                       int sample_rate) {
  if (!asmodeus_ear_init_done_) {
    asmodeus_ear_init_done_ = true;
    auto* cmd = base::CommandLine::ForCurrentProcess();
    if (!cmd->HasSwitch("asmodeus-device")) {
      return;
    }
    std::string name = cmd->GetSwitchValueASCII("asmodeus-device");
    const char* home = getenv("HOME");
    if (name.empty() || !home) {
      return;
    }
    std::string shm_path =
        std::string(home) + "/.asmodeus/audio-out-" + name + ".shm";
    int fd = open(shm_path.c_str(), O_RDWR);
    if (fd < 0) {
      LOG(ERROR) << "[Asmodeus] inbound-audio hook: cannot open " << shm_path
                 << " (ear ring must be created by the vat before spawn)";
      return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 24) {
      close(fd);
      return;
    }
    void* ptr =
        mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
      close(fd);
      return;
    }
    asmodeus_ear_fd_ = fd;
    asmodeus_ear_map_ = ptr;
    asmodeus_ear_map_size_ = st.st_size;
    UNSAFE_BUFFERS({
      asmodeus_ear_header_ = static_cast<uint32_t*>(ptr);
      asmodeus_ear_samples_ =
          reinterpret_cast<float*>(static_cast<uint8_t*>(ptr) + 24);
    });
    asmodeus_ear_capacity_ = (st.st_size - 24) / sizeof(float);
    LOG(WARNING) << "[Asmodeus] inbound-audio engine hook → " << shm_path
                 << " capacity=" << asmodeus_ear_capacity_
                 << " rate=" << sample_rate;
  }
  if (!asmodeus_ear_samples_ || asmodeus_ear_capacity_ == 0) {
    return;
  }
  const int channels = bus.channels();
  const int frames = bus.frames();
  if (channels <= 0 || frames <= 0) {
    return;
  }
  UNSAFE_BUFFERS({
    // Stamp the engine sample rate (the §5.2 engine_rate carrier) + mono.
    asmodeus_ear_header_[0] = static_cast<uint32_t>(sample_rate);
    asmodeus_ear_header_[1] = 1u;
    uint32_t wp = __atomic_load_n(&asmodeus_ear_header_[2], __ATOMIC_RELAXED);
    for (int i = 0; i < frames; ++i) {
      // Down-mix to mono. RAW decoded level — no gain, no AGC (§5.2). The
      // outbound MicBridge peak-normalization is the send-side level point.
      float sample = 0.0f;
      for (int ch = 0; ch < channels; ++ch) {
        sample += bus.channel(ch)[i];
      }
      sample /= static_cast<float>(channels);
      if (sample > 1.0f) {
        sample = 1.0f;
      } else if (sample < -1.0f) {
        sample = -1.0f;
      }
      asmodeus_ear_samples_[wp % asmodeus_ear_capacity_] = sample;
      ++wp;
    }
    __atomic_store_n(&asmodeus_ear_header_[2], wp, __ATOMIC_RELEASE);
  });
}

}  // namespace blink
