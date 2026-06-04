// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/media/media_handles.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aurelian/handles/media/media_seam.h"
#include "aurelian/handles/streams/subscription_producer.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

namespace {

using velite::agentspaces::Handle;
using velite::agentspaces::StateKind;
using velite::agentspaces::Value;
using velite::agentspaces::ValueHandle;

// legion://chrome/media/camera — the video_sink. The avatar tells frames in;
// they land in the MediaSeam the content-layer capture device reads.
class CameraSinkHandle : public Handle {
 public:
  explicit CameraSinkHandle(MediaSeam* seam) : seam_(seam) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/media/camera";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(
          Value(std::string("legion://chrome/media/camera")));
    }
    if (msg == "describe") {
      return ValueHandle::make(Value::make_object({
          {"kind", Value(std::string("video_sink"))},
          {"uri", Value(std::string("legion://chrome/media/camera"))},
      }));
    }
    if (msg == "frameCount") {
      return ValueHandle::make(
          Value(static_cast<int64_t>(seam_->video_frame_count())));
    }
    return ValueHandle::make_broken("unknown-message");
  }

  // The video_sink contract: tell("frame", {w,h,fmt,pixels,ts}).
  void tell(std::string_view msg, const Value& data) override {
    if (msg != "frame" || !data.is_object()) {
      return;
    }
    InjectedVideoFrame frame;
    if (const Value* w = data.object_get("w"); w && w->is_int()) {
      frame.width = static_cast<int>(w->as_int());
    }
    if (const Value* h = data.object_get("h"); h && h->is_int()) {
      frame.height = static_cast<int>(h->as_int());
    }
    if (const Value* f = data.object_get("fmt"); f && f->is_string()) {
      frame.fmt = f->as_string();
    }
    if (const Value* p = data.object_get("pixels"); p && p->is_bytes()) {
      frame.pixels = p->as_bytes();
    }
    if (const Value* t = data.object_get("ts"); t && t->is_int()) {
      frame.ts_micros = t->as_int();
    }
    seam_->PushVideoFrame(std::move(frame));
  }

 private:
  MediaSeam* seam_;
  Value identity_{std::string("legion://chrome/media/camera")};
};

// legion://chrome/media/mic — the audio_sink. TTS tells PCM frames in.
class MicSinkHandle : public Handle {
 public:
  explicit MicSinkHandle(MediaSeam* seam) : seam_(seam) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/media/mic";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(
          Value(std::string("legion://chrome/media/mic")));
    }
    if (msg == "describe") {
      return ValueHandle::make(Value::make_object({
          {"kind", Value(std::string("audio_sink"))},
          {"uri", Value(std::string("legion://chrome/media/mic"))},
      }));
    }
    if (msg == "pendingFrames") {
      return ValueHandle::make(
          Value(static_cast<int64_t>(seam_->pending_audio_frames())));
    }
    return ValueHandle::make_broken("unknown-message");
  }

  // The audio_sink contract: tell("frame", {pcm}) / tell("flush").
  void tell(std::string_view msg, const Value& data) override {
    if (msg == "flush") {
      seam_->FlushAudio();
      return;
    }
    if (msg != "frame" || !data.is_object()) {
      return;
    }
    if (const Value* pcm = data.object_get("pcm"); pcm && pcm->is_bytes()) {
      seam_->PushAudioFrame(pcm->as_bytes());
    }
  }

 private:
  MediaSeam* seam_;
  Value identity_{std::string("legion://chrome/media/mic")};
};

// legion://chrome/media/peer-audio — the audio_source. Captured peer voices
// fan out to every subscriber sink (Cicero's audio_source).
class PeerAudioSourceHandle : public Handle {
 public:
  PeerAudioSourceHandle() = default;

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override {
    return "legion://chrome/media/peer-audio";
  }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& spec) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(
          Value(std::string("legion://chrome/media/peer-audio")));
    }
    if (msg == "describe") {
      return ValueHandle::make(Value::make_object({
          {"kind", Value(std::string("audio_source"))},
          {"uri", Value(std::string("legion://chrome/media/peer-audio"))},
      }));
    }
    // The audio_source contract: subscribe({sink}) -> a SubscriptionHandle that
    // receives every broadcast peer-audio frame as a legion-notify tell.
    if (msg == "subscribe") {
      if (!spec.is_object()) {
        return ValueHandle::make_broken("subscribe-needs-sink");
      }
      const Value* sink_v = spec.object_get("sink");
      if (!sink_v || !sink_v->is_handle() || !sink_v->as_handle()) {
        return ValueHandle::make_broken("subscribe-needs-sink");
      }
      return producer_.Subscribe(sink_v->as_handle(),
                                 "legion://chrome/media/peer-audio", "");
    }
    if (msg == "subscriberCount") {
      return ValueHandle::make(
          Value(static_cast<int64_t>(producer_.active_count())));
    }
    return ValueHandle::make_broken("unknown-message");
  }

  // The capture side pushes peer voices in: tell("frame", {pcm}) fans out.
  void tell(std::string_view msg, const Value& data) override {
    if (msg == "frame") {
      producer_.Broadcast(data);
    }
  }

 private:
  SubscriptionProducer producer_;
  Value identity_{std::string("legion://chrome/media/peer-audio")};
};

// legion://chrome/media — routes to the three media contracts. Owns the
// children so their state (and the peer-audio subscriptions) persist across
// asks; the seam is shared (production: the global the capture device reads).
class MediaHandle : public Handle {
 public:
  explicit MediaHandle(MediaSeam* seam)
      : camera_(std::make_shared<CameraSinkHandle>(seam)),
        mic_(std::make_shared<MicSinkHandle>(seam)),
        peer_audio_(std::make_shared<PeerAudioSourceHandle>()) {}

  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const Value& resolved_value() const override { return identity_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::string sturdy_identity() const override { return "legion://chrome/media"; }

  std::shared_ptr<Handle> ask_impl(std::string_view msg,
                                   const Value& /*spec*/) override {
    if (msg == "__getIdentity") {
      return ValueHandle::make(Value(std::string("legion://chrome/media")));
    }
    if (msg == "describe") {
      return ValueHandle::make(Value::make_object({
          {"kind", Value(std::string("media"))},
          {"uri", Value(std::string("legion://chrome/media"))},
          {"camera", Value(std::string("video_sink"))},
          {"mic", Value(std::string("audio_sink"))},
          {"peer-audio", Value(std::string("audio_source"))},
      }));
    }
    const std::string name(msg);
    if (name == "camera") {
      return camera_;
    }
    if (name == "mic") {
      return mic_;
    }
    if (name == "peer-audio" || name == "peerAudio") {
      return peer_audio_;
    }
    if (!name.empty() && name.front() == '_') {
      return ValueHandle::make_broken("unknown-message");
    }
    return ValueHandle::make_broken("out-of-scope");
  }

  void tell(std::string_view, const Value&) override {}

 private:
  std::shared_ptr<Handle> camera_;
  std::shared_ptr<Handle> mic_;
  std::shared_ptr<Handle> peer_audio_;
  Value identity_{std::string("legion://chrome/media")};
};

}  // namespace

std::shared_ptr<Handle> CreateMediaHandle(MediaSeam* seam) {
  return std::make_shared<MediaHandle>(seam);
}

}  // namespace aurelian
