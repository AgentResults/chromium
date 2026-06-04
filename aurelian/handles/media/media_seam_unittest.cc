// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C-MEDIA-1 — the bindable media Handle surface over the MediaSeam.
//
// RED-first: with CameraSinkHandle::tell / MicSinkHandle::tell not routing into
// the seam, the frame-injection assertions fail (no frame buffered, queue
// empty). Routing the contract verbs into the MediaSeam makes them GREEN. This
// is the substrate-local seam the content-layer capture device (C-MEDIA-2)
// reads — proven here end-to-end through the real Handles + a real
// SubscriptionProducer, no mocks.

#include "aurelian/handles/media/media_handles.h"
#include "aurelian/handles/media/media_seam.h"

#include <memory>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {
namespace {

using V = velite::agentspaces::Value;
using Handle = velite::agentspaces::Handle;
using StateKind = velite::agentspaces::StateKind;

// A sink Handle that records every legion-notify frame (peer-audio subscriber).
class RecordingSink : public Handle {
 public:
  static std::shared_ptr<RecordingSink> make() {
    return std::shared_ptr<RecordingSink>(new RecordingSink());
  }
  StateKind state_kind() const override { return StateKind::ResolvedValue; }
  const V& resolved_value() const override { return value_; }
  std::shared_ptr<Handle> resolved_handle() const override { return nullptr; }
  std::string_view broken_reason() const override { return ""; }
  std::shared_ptr<Handle> ask_impl(std::string_view, const V&) override {
    return velite::agentspaces::ValueHandle::make_broken("not-callable");
  }
  void tell(std::string_view msg, const V& data) override {
    if (msg == "legion-notify") {
      frames_.push_back(data);
    }
  }
  const std::vector<V>& frames() const { return frames_; }

 private:
  RecordingSink() : value_("sink") {}
  V value_;
  std::vector<V> frames_;
};

std::string DescribeKind(const std::shared_ptr<Handle>& h) {
  auto d = h->ask("describe", V());
  const V& v = d->resolved_value();
  const V* kind = v.is_object() ? v.object_get("kind") : nullptr;
  return (kind && kind->is_string()) ? kind->as_string() : std::string();
}

// The video_sink contract: tell a frame -> it lands in the seam the capture
// device reads.
TEST(AurelianMediaSeamTest, CameraSinkPushesFrameIntoSeam) {
  MediaSeam seam;
  auto media = CreateMediaHandle(&seam);
  auto camera = media->ask("camera", V());
  ASSERT_EQ(StateKind::ResolvedValue, camera->state_kind());
  EXPECT_EQ("video_sink", DescribeKind(camera));

  std::vector<uint8_t> pixels = {1, 2, 3, 4, 5, 6};
  camera->tell("frame", V::make_object({
                            {"w", V(static_cast<int64_t>(320))},
                            {"h", V(static_cast<int64_t>(240))},
                            {"fmt", V(std::string("I420"))},
                            {"pixels", V::make_bytes(pixels)},
                            {"ts", V(static_cast<int64_t>(111))},
                        }));

  InjectedVideoFrame got = seam.LatestVideoFrame();
  ASSERT_TRUE(got.valid) << "video_sink must buffer the frame in the seam";
  EXPECT_EQ(320, got.width);
  EXPECT_EQ(240, got.height);
  EXPECT_EQ("I420", got.fmt);
  EXPECT_EQ(pixels, got.pixels);
  EXPECT_EQ(111, got.ts_micros);
  EXPECT_EQ(1u, seam.video_frame_count());

  auto count = camera->ask("frameCount", V());
  EXPECT_EQ(1, count->resolved_value().as_int());
}

// The audio_sink contract: queue PCM frames; flush drops them.
TEST(AurelianMediaSeamTest, MicSinkQueuesAndFlushes) {
  MediaSeam seam;
  auto media = CreateMediaHandle(&seam);
  auto mic = media->ask("mic", V());
  EXPECT_EQ("audio_sink", DescribeKind(mic));

  mic->tell("frame", V::make_object({{"pcm", V::make_bytes({10, 11})}}));
  mic->tell("frame", V::make_object({{"pcm", V::make_bytes({12, 13, 14})}}));
  EXPECT_EQ(2u, seam.pending_audio_frames());
  EXPECT_EQ(2, mic->ask("pendingFrames", V())->resolved_value().as_int());

  auto drained = seam.DrainAudio();
  ASSERT_EQ(2u, drained.size());
  EXPECT_EQ((std::vector<uint8_t>{10, 11}), drained[0]);
  EXPECT_EQ((std::vector<uint8_t>{12, 13, 14}), drained[1]);

  mic->tell("frame", V::make_object({{"pcm", V::make_bytes({99})}}));
  EXPECT_EQ(1u, seam.pending_audio_frames());
  mic->tell("flush", V());
  EXPECT_EQ(0u, seam.pending_audio_frames());
}

// The audio_source contract: subscribe a sink, then captured frames fan out.
TEST(AurelianMediaSeamTest, PeerAudioSourceFansOutToSubscriber) {
  MediaSeam seam;
  auto media = CreateMediaHandle(&seam);
  auto peer = media->ask("peer-audio", V());
  EXPECT_EQ("audio_source", DescribeKind(peer));

  auto sink = RecordingSink::make();
  auto sub = peer->ask(
      "subscribe", V::make_object({{"sink", V(std::shared_ptr<Handle>(sink))}}));
  ASSERT_NE(StateKind::Broken, sub->state_kind())
      << "subscribe with a sink must mint a subscription";
  EXPECT_EQ(1, peer->ask("subscriberCount", V())->resolved_value().as_int());

  peer->tell("frame", V(std::string("room-audio-1")));
  ASSERT_EQ(1u, sink->frames().size())
      << "captured peer frame must fan out to the audio_source subscriber";
  EXPECT_EQ("room-audio-1", sink->frames()[0].as_string());
}

// subscribe without a sink is refused (no silent half-subscription).
TEST(AurelianMediaSeamTest, PeerAudioSubscribeWithoutSinkRefused) {
  MediaSeam seam;
  auto media = CreateMediaHandle(&seam);
  auto peer = media->ask("peer-audio", V());
  auto sub = peer->ask("subscribe", V());
  EXPECT_EQ(StateKind::Broken, sub->state_kind());
  EXPECT_EQ(0, peer->ask("subscriberCount", V())->resolved_value().as_int());
}

// The mounted media handle advertises all three endowment contracts.
TEST(AurelianMediaSeamTest, MediaHandleDescribesEndowmentContracts) {
  MediaSeam seam;
  auto media = CreateMediaHandle(&seam);
  EXPECT_EQ("media", DescribeKind(media));
  EXPECT_EQ("legion://chrome/media",
            media->ask("__getIdentity", V())->resolved_value().as_string());
  EXPECT_EQ("video_sink", DescribeKind(media->ask("camera", V())));
  EXPECT_EQ("audio_sink", DescribeKind(media->ask("mic", V())));
  EXPECT_EQ("audio_source", DescribeKind(media->ask("peer-audio", V())));
}

}  // namespace
}  // namespace aurelian
