// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// NOLINTBEGIN(unsafe-buffers)

#include "media/audio/asmodeus/audio_ring_buffer.h"

#include <cmath>
#include <thread>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace asmodeus {

class AudioRingBufferTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    shm_path_ = temp_dir_.GetPath().Append("test.shm").value();
  }

  base::ScopedTempDir temp_dir_;
  std::string shm_path_;
};

TEST_F(AudioRingBufferTest, CreateAndOpen) {
  AudioRingBuffer writer;
  ASSERT_TRUE(writer.Create(shm_path_, 48000, 1, 48000));
  EXPECT_EQ(writer.sample_rate(), 48000);
  EXPECT_EQ(static_cast<int>(writer.Capacity()), 48000);
  writer.Close();

  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(shm_path_));
  EXPECT_EQ(reader.sample_rate(), 48000);
  EXPECT_EQ(static_cast<int>(reader.Available()), 0);
  reader.Close();
}

TEST_F(AudioRingBufferTest, WriteReadBasic) {
  AudioRingBuffer writer;
  ASSERT_TRUE(writer.Create(shm_path_, 48000, 1, 4800));

  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(shm_path_));

  std::vector<float> write_buf(480, 0.5f);
  writer.Write(write_buf.data(), 480);
  EXPECT_EQ(static_cast<int>(reader.Available()), 480);

  std::vector<float> read_buf(480);
  size_t nread = reader.Read(read_buf.data(), 480);
  EXPECT_EQ(static_cast<int>(nread), 480);
  for (int i = 0; i < 480; i++) {
    EXPECT_FLOAT_EQ(read_buf[i], 0.5f);
  }
}

TEST_F(AudioRingBufferTest, WriteReadSineWave) {
  AudioRingBuffer writer;
  ASSERT_TRUE(writer.Create(shm_path_, 48000, 1, 48000));

  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(shm_path_));

  const size_t kN = 48000;
  std::vector<float> sine(kN);
  for (size_t i = 0; i < kN; i++) {
    sine[i] = static_cast<float>(
        std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / 48000.0));
  }
  writer.Write(sine.data(), kN);

  // Read back all at once
  std::vector<float> output(kN);
  size_t total_read = reader.Read(output.data(), kN);
  ASSERT_EQ(total_read, kN);

  for (size_t i = 0; i < kN; i++) {
    EXPECT_NEAR(output[i], sine[i], 1e-6f) << "Sample " << i;
  }
}

TEST_F(AudioRingBufferTest, OverflowSkipsToNearHead) {
  AudioRingBuffer writer;
  ASSERT_TRUE(writer.Create(shm_path_, 48000, 1, 4800));

  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(shm_path_));

  std::vector<float> buf(9600);
  for (size_t i = 0; i < 9600; i++)
    buf[i] = static_cast<float>(i);
  writer.Write(buf.data(), 9600);

  size_t avail = reader.Available();
  EXPECT_LE(static_cast<int>(avail), 4800);

  std::vector<float> out(avail);
  size_t n = reader.Read(out.data(), avail);
  EXPECT_EQ(n, avail);
  if (n > 0) {
    EXPECT_GT(out[n - 1], 4800.0f);
  }
}

TEST_F(AudioRingBufferTest, CreateIfNeededIdempotent) {
  AudioRingBuffer buf;
  ASSERT_TRUE(buf.CreateIfNeeded(shm_path_, 48000, 1, 48000));

  std::vector<float> data(100, 0.42f);
  buf.Write(data.data(), 100);
  buf.Close();

  AudioRingBuffer buf2;
  ASSERT_TRUE(buf2.CreateIfNeeded(shm_path_, 48000, 1, 48000));
  EXPECT_EQ(buf2.sample_rate(), 48000);
  buf2.Close();
}

TEST_F(AudioRingBufferTest, MultipleChunkedWrites) {
  AudioRingBuffer writer;
  ASSERT_TRUE(writer.Create(shm_path_, 48000, 1, 48000));

  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(shm_path_));

  for (int chunk = 0; chunk < 10; chunk++) {
    float val = static_cast<float>(chunk) / 10.0f;
    std::vector<float> buf(480, val);
    writer.Write(buf.data(), 480);
  }

  EXPECT_EQ(static_cast<int>(reader.Available()), 4800);

  std::vector<float> out(4800);
  size_t n = reader.Read(out.data(), 4800);
  EXPECT_EQ(static_cast<int>(n), 4800);

  EXPECT_FLOAT_EQ(out[0], 0.0f);
  EXPECT_FLOAT_EQ(out[4320], 0.9f);
}

TEST_F(AudioRingBufferTest, SineWaveSignalIntegrity) {
  AudioRingBuffer writer;
  ASSERT_TRUE(writer.Create(shm_path_, 48000, 1, 48000));

  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(shm_path_));

  const size_t kN = 48000;
  std::vector<float> original(kN);
  for (size_t i = 0; i < kN; i++) {
    original[i] = static_cast<float>(
        std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / 48000.0) *
        0.8);
  }

  // Write in 10ms chunks
  for (size_t offset = 0; offset < kN; offset += 480) {
    writer.Write(&original[offset], 480);
  }

  // Read in 10ms chunks
  std::vector<float> recovered(kN);
  size_t total = 0;
  while (total < kN) {
    size_t n = reader.Read(&recovered[total], 480);
    if (n == 0)
      break;
    total += n;
  }
  ASSERT_EQ(total, kN);

  double signal_power = 0, noise_power = 0;
  for (size_t i = 0; i < kN; i++) {
    signal_power += original[i] * original[i];
    float error = recovered[i] - original[i];
    noise_power += error * error;
  }
  signal_power /= static_cast<double>(kN);
  noise_power /= static_cast<double>(kN);

  if (noise_power > 0) {
    double snr_db = 10.0 * std::log10(signal_power / noise_power);
    EXPECT_GT(snr_db, 90.0) << "SNR too low: " << snr_db << " dB";
  }
}

TEST_F(AudioRingBufferTest, LatencyUnderOneMs) {
  AudioRingBuffer writer;
  ASSERT_TRUE(writer.Create(shm_path_, 48000, 1, 48000));

  AudioRingBuffer reader;
  ASSERT_TRUE(reader.Open(shm_path_));

  auto t0 = std::chrono::steady_clock::now();
  std::vector<float> buf(480, 0.5f);
  writer.Write(buf.data(), 480);

  std::vector<float> out(480);
  size_t n = reader.Read(out.data(), 480);
  auto t1 = std::chrono::steady_clock::now();

  EXPECT_EQ(static_cast<int>(n), 480);
  auto latency_us =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  EXPECT_LT(latency_us, 1000) << "Write+read latency " << latency_us << "us";
}

}  // namespace asmodeus

// NOLINTEND(unsafe-buffers)
