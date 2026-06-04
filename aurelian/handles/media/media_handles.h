// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_HANDLES_MEDIA_MEDIA_HANDLES_H_
#define AURELIAN_HANDLES_MEDIA_MEDIA_HANDLES_H_

#include <memory>

namespace velite::agentspaces {
class Handle;
}  // namespace velite::agentspaces

namespace aurelian {

class MediaSeam;

// The mountable legion://chrome/media handle — the bindable media surface a
// Cicero endowment binds to (AURELIAN-DESIGN §12 / Cicero §26). It exposes
// three child contracts:
//
//   legion://chrome/media/camera      video_sink  tell("frame",{w,h,fmt,pixels,ts})
//   legion://chrome/media/mic         audio_sink  tell("frame",{pcm}) / tell("flush")
//   legion://chrome/media/peer-audio  audio_source ask("subscribe",{sink}) / tell("frame")
//
// camera + mic WRITE the avatar/TTS frames into `seam` (the content-layer
// capture device reads them and surfaces them as the real getUserMedia
// camera/mic). peer-audio fans captured peer voices out to subscribers via the
// in-process SubscriptionProducer (the audio_source Cicero hears the room on).
//
// The returned handle and its children share `seam` — in production
// `&MediaSeam::Get()` (the singleton the capture device reads); a test passes a
// local MediaSeam for isolation.
std::shared_ptr<velite::agentspaces::Handle> CreateMediaHandle(MediaSeam* seam);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_MEDIA_MEDIA_HANDLES_H_
