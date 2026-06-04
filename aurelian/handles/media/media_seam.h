// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_HANDLES_MEDIA_MEDIA_SEAM_H_
#define AURELIAN_HANDLES_MEDIA_MEDIA_SEAM_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"

namespace aurelian {

// One injected video frame (the avatar's rendered face). `pixels` is raw bytes
// in `fmt` (e.g. "I420" planar or "RGBA" packed); the content-layer capture
// device transcodes to the media::VideoFrame it hands getUserMedia.
struct InjectedVideoFrame {
  int width = 0;
  int height = 0;
  std::string fmt;
  std::vector<uint8_t> pixels;
  int64_t ts_micros = 0;
  bool valid = false;
};

// MediaSeam — the process-global media-injection buffer (browser process). It
// is the honest hand-off point between the Velite media Handles and the
// Chromium content-layer capture device (AURELIAN-DESIGN §12 / C-MEDIA):
//
//   video_sink Handle  --PushVideoFrame-->  MediaSeam  --LatestVideoFrame-->
//       (avatar, UI thread)                              Aurelian capture device
//                                                        (media/, capture thread)
//
// The Handle WRITES agent frames; the capture device READS them and delivers
// them as a real camera/mic into getUserMedia. Both run in the browser process
// but on different threads, so every accessor is lock-guarded.
//
// This is the substrate-local seam. The content-layer device + device-enum +
// permission auto-grant that consume it are C-MEDIA-2/3/4.
class MediaSeam {
 public:
  // The process singleton the production capture device + mounted Handles share.
  static MediaSeam& Get();

  MediaSeam();
  ~MediaSeam();

  MediaSeam(const MediaSeam&) = delete;
  MediaSeam& operator=(const MediaSeam&) = delete;

  // --- video_sink (camera) ---
  // Replaces the latest frame (camera capture is latest-wins, not a queue).
  void PushVideoFrame(InjectedVideoFrame frame);
  // The most recent frame, or {valid=false} if none pushed yet.
  InjectedVideoFrame LatestVideoFrame() const;
  uint64_t video_frame_count() const;

  // --- audio_sink (mic) ---
  // Appends a PCM frame (mic capture is a queue the device drains in order).
  void PushAudioFrame(std::vector<uint8_t> pcm);
  // Drops every queued frame (tell("flush") — e.g. on utterance boundary).
  void FlushAudio();
  // Removes and returns every queued PCM frame in arrival order.
  std::vector<std::vector<uint8_t>> DrainAudio();
  size_t pending_audio_frames() const;

 private:
  mutable base::Lock lock_;
  InjectedVideoFrame latest_video_ GUARDED_BY(lock_);
  uint64_t video_count_ GUARDED_BY(lock_) = 0;
  std::vector<std::vector<uint8_t>> audio_queue_ GUARDED_BY(lock_);
};

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_MEDIA_MEDIA_SEAM_H_
