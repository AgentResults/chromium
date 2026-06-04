// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/profile/audio_capture_handle.h"

#include <map>
#include <memory>

#include "chrome/browser/asmodeus/asmodeus_audio_capture.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// A Velite handle that owns the real asmodeus::AsmodeusAudioCapture bound to a
// WebContents and exposes start/level/stop through ask dispatch.
class AudioCaptureHandle : public Handle {
 public:
  explicit AudioCaptureHandle(content::WebContents* wc) : wc_(wc) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/browser/tabs/audio/capture";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "start") {
      std::string path;
      int sr = 48000;
      int ch = 1;
      if (spec.is_object()) {
        const auto& o = spec.as_object();
        if (auto it = o.find("path");
            it != o.end() && it->second.is_string()) {
          path = it->second.as_string();
        }
        if (auto it = o.find("sampleRate");
            it != o.end() && it->second.is_int()) {
          sr = static_cast<int>(it->second.as_int());
        }
        if (auto it = o.find("channels");
            it != o.end() && it->second.is_int()) {
          ch = static_cast<int>(it->second.as_int());
        }
      }
      return ValueHandle::make(Value(capture_.Start(wc_, path, sr, ch)));
    }
    if (msg == "level") {
      asmodeus::AsmodeusAudioCapture::AudioLevel lvl = capture_.GetLevel();
      return ValueHandle::make(Value::make_object({
          {"rms", Value(lvl.rms)},
          {"peak", Value(lvl.peak)},
          {"capturing", Value(lvl.capturing)},
      }));
    }
    if (msg == "stop") {
      asmodeus::AsmodeusAudioCapture::CaptureResult r = capture_.Stop();
      return ValueHandle::make(Value::make_object({
          {"durationMs", Value(r.duration_ms)},
          {"samples", Value(static_cast<int64_t>(r.samples))},
          {"peakRms", Value(r.peak_rms)},
          {"outputPath", Value(r.output_path)},
      }));
    }
    return ValueHandle::make_broken("not-callable");
  }

  void tell(std::string_view /*msg*/, const Value& /*data*/) override {}

 private:
  content::WebContents* wc_;
  asmodeus::AsmodeusAudioCapture capture_;
  Value identity_{std::string("legion://chrome/browser/tabs/audio/capture")};
};

int64_t FieldInt(const std::map<std::string, Value>& o, const std::string& k) {
  auto it = o.find(k);
  return (it != o.end() && it->second.is_int()) ? it->second.as_int() : 0;
}

double FieldDouble(const std::map<std::string, Value>& o,
                   const std::string& k) {
  auto it = o.find(k);
  if (it == o.end()) return 0;
  if (it->second.is_double()) return it->second.as_double();
  if (it->second.is_int()) return static_cast<double>(it->second.as_int());
  return 0;
}

bool FieldBool(const std::map<std::string, Value>& o, const std::string& k) {
  auto it = o.find(k);
  return it != o.end() && it->second.is_bool() && it->second.as_bool();
}

std::string FieldString(const std::map<std::string, Value>& o,
                        const std::string& k) {
  auto it = o.find(k);
  return (it != o.end() && it->second.is_string()) ? it->second.as_string()
                                                   : std::string();
}

}  // namespace

// The opaque session: owns the Velite handle behind the C-API.
struct AudioCaptureSession {
  std::shared_ptr<velite::agentspaces::Handle> handle;
};

AudioCaptureSession* CreateAudioCaptureHandle(content::WebContents* wc) {
  auto* session = new AudioCaptureSession();
  session->handle = std::make_shared<AudioCaptureHandle>(wc);
  return session;
}

void DestroyAudioCaptureHandle(AudioCaptureSession* handle) {
  delete handle;
}

bool AudioCaptureHandleStart(AudioCaptureSession* holder,
                             const std::string& output_path,
                             int sample_rate,
                             int channels) {
  if (!holder || !holder->handle) {
    return false;
  }
  Value spec = Value::make_object({
      {"path", Value(output_path)},
      {"sampleRate", Value(sample_rate)},
      {"channels", Value(channels)},
  });
  std::shared_ptr<Handle> result = holder->handle->ask("start", spec);
  return result && result->state_kind() == StateKind::ResolvedValue &&
         result->resolved_value().is_bool() &&
         result->resolved_value().as_bool();
}

AudioCaptureStats AudioCaptureHandleStop(AudioCaptureSession* holder) {
  AudioCaptureStats stats;
  if (!holder || !holder->handle) {
    return stats;
  }
  std::shared_ptr<Handle> result = holder->handle->ask("stop", Value());
  if (!result || result->state_kind() != StateKind::ResolvedValue ||
      !result->resolved_value().is_object()) {
    return stats;
  }
  const auto& o = result->resolved_value().as_object();
  stats.duration_ms = FieldDouble(o, "durationMs");
  stats.samples = FieldInt(o, "samples");
  stats.peak_rms = FieldDouble(o, "peakRms");
  stats.output_path = FieldString(o, "outputPath");
  return stats;
}

void AudioCaptureHandleLevel(AudioCaptureSession* holder,
                             double* rms,
                             double* peak,
                             bool* capturing) {
  *rms = 0;
  *peak = 0;
  *capturing = false;
  if (!holder || !holder->handle) {
    return;
  }
  std::shared_ptr<Handle> result = holder->handle->ask("level", Value());
  if (!result || result->state_kind() != StateKind::ResolvedValue ||
      !result->resolved_value().is_object()) {
    return;
  }
  const auto& o = result->resolved_value().as_object();
  *rms = FieldDouble(o, "rms");
  *peak = FieldDouble(o, "peak");
  *capturing = FieldBool(o, "capturing");
}

}  // namespace aurelian
