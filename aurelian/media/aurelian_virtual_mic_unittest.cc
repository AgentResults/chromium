// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C-MEDIA-2d (RED-first): the TTS virtual mic producer. Cicero's
// audio_sink writes TTS PCM into MediaSeam; AurelianVirtualMic drains it into
// the shared-memory ring buffer the fork's FakeAudioInputStream reads as the
// browser microphone. This verifies the producer writes the EXACT ring format
// the reader depends on (header offsets + float samples + write_pos advance) —
// deterministic, no beep/audio-processing flakiness. The live getUserMedia-audio
// audible proof is the C-MEDIA-4 SIT.

#include "aurelian/media/aurelian_virtual_mic.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "aurelian/handles/media/media_seam.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {
namespace {

std::vector<uint8_t> FloatsToBytes(const std::vector<float>& f) {
  std::vector<uint8_t> b(f.size() * sizeof(float));
  std::memcpy(b.data(), f.data(), b.size());
  return b;
}

uint32_t ReadU32(const std::string& raw, size_t offset) {
  uint32_t v = 0;
  std::memcpy(&v, raw.data() + offset, sizeof(v));
  return v;
}

float ReadSample(const std::string& raw, size_t index) {
  float v = 0.0f;
  std::memcpy(&v, raw.data() + 24 + index * sizeof(float), sizeof(v));
  return v;
}

// The producer drains MediaSeam audio into the ring in the reader's exact
// format: sr@0, ch@4, write_pos@8, then float32 samples @24.
TEST(AurelianVirtualMicTest, DrainsMediaSeamAudioIntoTheRing) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const std::string path = dir.GetPath().AppendASCII("audio-in.shm").value();
  MediaSeam::Get().FlushAudio();

  AurelianVirtualMic mic;
  ASSERT_TRUE(mic.OpenRingForTesting(path));

  // Cicero's audio_sink writes TTS PCM (float32) into the seam.
  const std::vector<float> tts = {0.25f, 0.5f, 0.75f, -0.5f, 0.125f};
  MediaSeam::Get().PushAudioFrame(FloatsToBytes(tts));

  mic.PumpOnce();

  std::string raw;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(path), &raw));
  ASSERT_GE(raw.size(), 24u + tts.size() * sizeof(float));

  EXPECT_EQ(48000u, ReadU32(raw, 0)) << "sample_rate";
  EXPECT_EQ(1u, ReadU32(raw, 4)) << "channels";
  EXPECT_EQ(tts.size(), ReadU32(raw, 8))
      << "write_pos must advance by the sample count the reader will consume";
  for (size_t i = 0; i < tts.size(); ++i) {
    EXPECT_FLOAT_EQ(tts[i], ReadSample(raw, i)) << "sample " << i;
  }
}

// Successive pumps append; write_pos keeps advancing across frames.
TEST(AurelianVirtualMicTest, SuccessivePumpsAppend) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const std::string path = dir.GetPath().AppendASCII("audio-in.shm").value();
  MediaSeam::Get().FlushAudio();

  AurelianVirtualMic mic;
  ASSERT_TRUE(mic.OpenRingForTesting(path));

  MediaSeam::Get().PushAudioFrame(FloatsToBytes({1.0f, 2.0f}));
  mic.PumpOnce();
  MediaSeam::Get().PushAudioFrame(FloatsToBytes({3.0f}));
  mic.PumpOnce();

  std::string raw;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(path), &raw));
  EXPECT_EQ(3u, ReadU32(raw, 8));
  EXPECT_FLOAT_EQ(3.0f, ReadSample(raw, 2));
}

// An empty seam is a no-op: no samples, write_pos stays 0.
TEST(AurelianVirtualMicTest, EmptySeamIsNoOp) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const std::string path = dir.GetPath().AppendASCII("audio-in.shm").value();
  MediaSeam::Get().FlushAudio();

  AurelianVirtualMic mic;
  ASSERT_TRUE(mic.OpenRingForTesting(path));
  mic.PumpOnce();

  std::string raw;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(path), &raw));
  EXPECT_EQ(0u, ReadU32(raw, 8));
}

}  // namespace
}  // namespace aurelian
