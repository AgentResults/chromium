// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/asmodeus/asmodeus_audio_output.h"

#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

// Test audio source that produces a constant value.
class TestAudioSource : public media::AudioOutputStream::AudioSourceCallback {
 public:
  explicit TestAudioSource(float value) : value_(value) {}

  int OnMoreData(base::TimeDelta delay,
                 base::TimeTicks delay_timestamp,
                 const media::AudioGlitchInfo& glitch_info,
                 media::AudioBus* dest) override {
    for (int ch = 0; ch < dest->channels(); ch++) {
      for (int i = 0; i < dest->frames(); i++) {
        dest->channel(ch)[i] = value_;
      }
    }
    return dest->frames();
  }

  void OnError(ErrorType type) override {}

 private:
  float value_;
};

class AsmodeusAudioOutputTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    out_path_ = temp_dir_.GetPath().Append("audio-out-test.shm").value();
    in_path_ = temp_dir_.GetPath().Append("audio-in-test.shm").value();

    // Pre-create SHM files
    AudioRingBuffer setup;
    ASSERT_TRUE(setup.Create(out_path_, 48000, 1, 48000 * 10));
    setup.Close();
    ASSERT_TRUE(setup.Create(in_path_, 48000, 1, 48000 * 10));
    setup.Close();
  }

  base::test::TaskEnvironment task_env_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir temp_dir_;
  std::string out_path_;
  std::string in_path_;
};

TEST_F(AsmodeusAudioOutputTest, OpensWithValidPath) {
  media::AudioParameters params(
      media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
      media::ChannelLayoutConfig::Mono(), 48000, 480);
  auto* output = new AsmodeusAudioOutput(params, out_path_);
  EXPECT_TRUE(output->Open());
  output->Close();  // Close calls delete this
}

TEST_F(AsmodeusAudioOutputTest, NoOpWithEmptyPath) {
  media::AudioParameters params(
      media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
      media::ChannelLayoutConfig::Mono(), 48000, 480);
  auto* output = new AsmodeusAudioOutput(params, "");
  EXPECT_TRUE(output->Open());
  output->Close();
}

TEST_F(AsmodeusAudioOutputTest, WritesToSHMOnTimer) {
  media::AudioParameters params(
      media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
      media::ChannelLayoutConfig::Mono(), 48000, 480);
  auto* output = new AsmodeusAudioOutput(params, out_path_);
  ASSERT_TRUE(output->Open());

  TestAudioSource source(0.1f);  // Low signal so gain doesn't clip
  output->Start(&source);

  // Advance time to trigger timer callbacks
  task_env_.FastForwardBy(base::Milliseconds(100));  // 10 timer firings

  output->Stop();

  // Verify SHM has data
  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(out_path_));
  EXPECT_GT(reader.Available(), 0u)
      << "SHM should have audio data after timer firings";

  // Read and verify content
  // 0.1 * kOutputGain(5.0) = 0.5
  std::vector<float> buf(480);
  reader.Read(buf.data(), 480);
  EXPECT_NEAR(buf[0], 0.5f, 0.05f);

  output->Close();
}

TEST_F(AsmodeusAudioOutputTest, EmptyPathDoesNotWriteToSHM) {
  media::AudioParameters params(
      media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
      media::ChannelLayoutConfig::Mono(), 48000, 480);
  auto* output = new AsmodeusAudioOutput(params, "");
  ASSERT_TRUE(output->Open());

  TestAudioSource source(0.5f);
  output->Start(&source);
  task_env_.FastForwardBy(base::Milliseconds(100));
  output->Stop();

  // out_path_ SHM should still be empty (output used empty path)
  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(out_path_));
  EXPECT_EQ(reader.Available(), 0u)
      << "No-op output should not write to any SHM";

  output->Close();
}

TEST_F(AsmodeusAudioOutputTest, VolumeControl) {
  media::AudioParameters params(
      media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
      media::ChannelLayoutConfig::Mono(), 48000, 480);
  auto* output = new AsmodeusAudioOutput(params, out_path_);
  ASSERT_TRUE(output->Open());

  double vol;
  output->SetVolume(0.5);
  output->GetVolume(&vol);
  EXPECT_DOUBLE_EQ(vol, 0.5);

  output->Close();
}

}  // namespace asmodeus
