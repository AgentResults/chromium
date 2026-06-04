// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C10: route the existing asmodeus tab-audio capture THROUGH a Velite
// handle (legion://chrome/browser/tabs/<id>/audio/capture). The handle binds a
// WebContents and owns the real asmodeus::AsmodeusAudioCapture; start/level/stop
// flow through its ask dispatch. The C-API shim keeps Velite types out of the
// Chrome (CDP handler) translation unit that drives it.

#ifndef AURELIAN_HANDLES_PROFILE_AUDIO_CAPTURE_HANDLE_H_
#define AURELIAN_HANDLES_PROFILE_AUDIO_CAPTURE_HANDLE_H_

#include <cstdint>
#include <memory>
#include <string>

namespace content {
class WebContents;
}

namespace aurelian {

// Opaque session owning the Velite audio-capture handle (defined in the .cc).
struct AudioCaptureSession;

// Creates a session bound to `wc` (which must outlive it).
AudioCaptureSession* CreateAudioCaptureHandle(content::WebContents* wc);
void DestroyAudioCaptureHandle(AudioCaptureSession* handle);

// RAII owner for a session: a unique_ptr whose deleter calls Destroy.
struct AudioCaptureSessionDeleter {
  void operator()(AudioCaptureSession* s) const { DestroyAudioCaptureHandle(s); }
};
using ScopedAudioCaptureSession =
    std::unique_ptr<AudioCaptureSession, AudioCaptureSessionDeleter>;

// Starts capture on the bound WebContents; returns false on failure (routed
// through the handle's ask("start")).
bool AudioCaptureHandleStart(AudioCaptureSession* handle,
                             const std::string& output_path,
                             int sample_rate,
                             int channels);

// Stops capture and returns the WAV stats (routed through ask("stop")).
struct AudioCaptureStats {
  double duration_ms = 0;
  int64_t samples = 0;
  double peak_rms = 0;
  std::string output_path;
};
AudioCaptureStats AudioCaptureHandleStop(AudioCaptureSession* handle);

// Reads the current audio level / capturing state (routed through ask("level")).
void AudioCaptureHandleLevel(AudioCaptureSession* handle,
                             double* rms,
                             double* peak,
                             bool* capturing);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_PROFILE_AUDIO_CAPTURE_HANDLE_H_
