// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/media/media_seam.h"

#include <utility>

#include "base/no_destructor.h"

namespace aurelian {

MediaSeam::MediaSeam() = default;
MediaSeam::~MediaSeam() = default;

// static
MediaSeam& MediaSeam::Get() {
  static base::NoDestructor<MediaSeam> instance;
  return *instance;
}

void MediaSeam::PushVideoFrame(InjectedVideoFrame frame) {
  frame.valid = true;
  base::AutoLock guard(lock_);
  latest_video_ = std::move(frame);
  ++video_count_;
}

InjectedVideoFrame MediaSeam::LatestVideoFrame() const {
  base::AutoLock guard(lock_);
  return latest_video_;
}

uint64_t MediaSeam::video_frame_count() const {
  base::AutoLock guard(lock_);
  return video_count_;
}

void MediaSeam::PushAudioFrame(std::vector<uint8_t> pcm) {
  base::AutoLock guard(lock_);
  audio_queue_.push_back(std::move(pcm));
}

void MediaSeam::FlushAudio() {
  base::AutoLock guard(lock_);
  audio_queue_.clear();
}

std::vector<std::vector<uint8_t>> MediaSeam::DrainAudio() {
  base::AutoLock guard(lock_);
  std::vector<std::vector<uint8_t>> out;
  out.swap(audio_queue_);
  return out;
}

size_t MediaSeam::pending_audio_frames() const {
  base::AutoLock guard(lock_);
  return audio_queue_.size();
}

}  // namespace aurelian
