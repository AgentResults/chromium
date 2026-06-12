// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/fake_audio_input_stream.h"

#include <memory>
#include <string>

#include "base/atomicops.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_split.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "media/audio/audio_manager_base.h"
#include "base/compiler_specific.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "media/audio/simple_sources.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/media_switches.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <cstring>

namespace media {

namespace {
std::atomic<bool> g_fake_input_streams_are_muted;
}

AudioInputStream* FakeAudioInputStream::MakeFakeStream(
    AudioManagerBase* manager,
    const AudioParameters& params) {
  return new FakeAudioInputStream(manager, params);
}

FakeAudioInputStream::FakeAudioInputStream(AudioManagerBase* manager,
                                           const AudioParameters& params)
    : audio_manager_(manager),
      callback_(nullptr),
      params_(params),
      audio_bus_(AudioBus::Create(params)),
      capture_thread_(
          nullptr,
          base::OnTaskRunnerDeleter(manager->GetWorkerTaskRunner())) {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
}

FakeAudioInputStream::~FakeAudioInputStream() {
  // |worker_| should be null as Stop() should have been called before.
  DCHECK(!capture_thread_);
  DCHECK(!callback_);
  DCHECK(!fake_audio_worker_);
}

AudioInputStream::OpenOutcome FakeAudioInputStream::Open() {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  audio_bus_->Zero();

  return OpenOutcome::kSuccess;
}

void FakeAudioInputStream::Start(AudioInputCallback* callback) {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  DCHECK(!capture_thread_);
  DCHECK(callback);
  DCHECK(!fake_audio_worker_);

  capture_thread_.reset(new base::Thread("FakeAudioInput"));
  // kRealtimeAudio priority is needed to avoid audio playout delays.
  // See crbug.com/971265
  CHECK(capture_thread_->StartWithOptions(
      base::Thread::Options(base::ThreadType::kRealtimeAudio)));

  {
    base::AutoLock lock(callback_lock_);
    DCHECK(!callback_);
    callback_ = callback;
  }

  fake_audio_worker_ = std::make_unique<FakeAudioWorker>(
      capture_thread_->task_runner(), params_);
  fake_audio_worker_->Start(base::BindRepeating(
      &FakeAudioInputStream::ReadAudioFromSource, base::Unretained(this)));
}

void FakeAudioInputStream::Stop() {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  // Start has not been called yet.
  if (!capture_thread_) {
    return;
  }

  {
    base::AutoLock lock(callback_lock_);
    DCHECK(callback_);
    callback_ = nullptr;
  }

  DCHECK(fake_audio_worker_);
  fake_audio_worker_->Stop();
  fake_audio_worker_.reset();

  capture_thread_.reset();
}

void FakeAudioInputStream::Close() {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  Stop();
  audio_manager_->ReleaseInputStream(this);
}

double FakeAudioInputStream::GetMaxVolume() {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  return 1.0;
}

void FakeAudioInputStream::SetVolume(double volume) {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
}

double FakeAudioInputStream::GetVolume() {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  return 1.0;
}

bool FakeAudioInputStream::IsMuted() {
  DCHECK(audio_manager_->GetTaskRunner()->BelongsToCurrentThread());
  return g_fake_input_streams_are_muted.load(std::memory_order_relaxed);
}

bool FakeAudioInputStream::SetAutomaticGainControl(bool enabled) {
  return false;
}

bool FakeAudioInputStream::GetAutomaticGainControl() {
  return false;
}

void FakeAudioInputStream::SetOutputDeviceForAec(
    const std::string& output_device_id) {
  // Not supported. Do nothing.
}

void FakeAudioInputStream::ReadAudioFromSource(base::TimeTicks ideal_time,
                                               base::TimeTicks now) {
  DCHECK(capture_thread_->task_runner()->BelongsToCurrentThread());

  if (!audio_source_)
    audio_source_ = ChooseSource();

  // This OnMoreData()/OnData() timing would never happen in a real system:
  //
  //   1. Real AudioSources would never be asked to generate audio that should
  //      already be playing-out exactly at this very moment; they are asked to
  //      do so for audio to be played-out in the future.
  //   2. Real AudioInputStreams could never provide audio that is striking a
  //      microphone element exactly at this very moment; they provide audio
  //      that happened in the recent past.
  //
  // However, it would be pointless to add a FIFO queue here to delay the signal
  // in this "fake" implementation. So, just hack the timing and carry-on.
  {
    base::AutoLock lock(callback_lock_);
    if (audio_bus_ && callback_) {
      audio_source_->OnMoreData(base::TimeDelta(), ideal_time, {},
                                audio_bus_.get());
      callback_->OnData(audio_bus_.get(), ideal_time, 1.0, {});
    }
  }
}

using AudioSourceCallback = AudioOutputStream::AudioSourceCallback;
// Asmodeus shared memory audio source.
// Reads PCM samples from a memory-mapped ring buffer file.
// Same layout as AudioRingBuffer in chrome/browser/asmodeus/.
namespace {

// Use ~/.asmodeus/ because /tmp is blocked by macOS sandbox.
std::string GetAsmodeusAudioInPath() {
  const char* home = getenv("HOME");
  if (!home) return "";
  // Use --asmodeus-device flag for per-agent shm path
  auto* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch(switches::kAsmodeusDevice)) {
    std::string name = cmd->GetSwitchValueASCII(switches::kAsmodeusDevice);
    return std::string(home) + "/.asmodeus/audio-in-" + name + ".shm";
  }
  return std::string(home) + "/.asmodeus/audio-in.shm";
}
constexpr size_t kShmHeaderSize = 24;  // Updated: sr(4) ch(4) wp(4) rp(4) ts(8)

class SharedMemorySource : public AudioOutputStream::AudioSourceCallback {
 public:
  SharedMemorySource(const AudioParameters& params, const std::string& path)
      : params_(params) {
    fd_ = open(path.c_str(), O_RDWR);
    if (fd_ < 0) return;
    struct stat st;
    if (fstat(fd_, &st) != 0 || st.st_size <= static_cast<off_t>(kShmHeaderSize)) {
      close(fd_); fd_ = -1; return;
    }
    mapped_size_ = static_cast<size_t>(st.st_size);
    mapped_ = mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped_ == MAP_FAILED) { mapped_ = nullptr; close(fd_); fd_ = -1; return; }
    buffer_samples_ = (mapped_size_ - kShmHeaderSize) / sizeof(float);
  }

  ~SharedMemorySource() override {
    if (mapped_) munmap(mapped_, mapped_size_);
    if (fd_ >= 0) close(fd_);
  }

  int OnMoreData(base::TimeDelta, base::TimeTicks,
                 const AudioGlitchInfo&, AudioBus* dest) override {
    if (!mapped_) { dest->Zero(); return dest->frames(); }

    // SAFETY: header points into mmap'd region of known size (>kShmHeaderSize).
    // Offsets 8 and 12 are within the 16-byte header. buffer starts after header.
    auto* header = static_cast<uint8_t*>(mapped_);
    uint32_t wp = UNSAFE_BUFFERS(reinterpret_cast<std::atomic<uint32_t>*>(header + 8))->load(std::memory_order_acquire);
    auto* rp_ptr = UNSAFE_BUFFERS(reinterpret_cast<std::atomic<uint32_t>*>(header + 12));
    uint32_t rp = rp_ptr->load(std::memory_order_relaxed);
    auto* buffer = UNSAFE_BUFFERS(reinterpret_cast<float*>(header + kShmHeaderSize));

    const int frames = dest->frames();
    const int channels = dest->channels();

    uint32_t available = wp - rp;
    if (available > static_cast<uint32_t>(buffer_samples_)) {
      rp = wp - static_cast<uint32_t>(buffer_samples_);
    }

    int samples_read = 0;
    for (int f = 0; f < frames; ++f) {
      float sample = 0.0f;
      if (rp < wp) {
        sample = UNSAFE_BUFFERS(buffer[rp % buffer_samples_]);
        ++rp;
        ++samples_read;
      }
      for (int ch = 0; ch < channels; ++ch) {
        dest->channel(ch)[static_cast<size_t>(f)] = sample;
      }
    }

    rp_ptr->store(rp, std::memory_order_release);

    // Log first few calls to verify data flow
    static int log_count = 0;
    if (log_count < 5) {
      float first = dest->channel(0)[0];
      LOG(WARNING) << "[Asmodeus] OnMoreData: frames=" << frames
                   << " wp=" << wp << " read=" << samples_read
                   << " first_sample=" << first
                   << " buffer_samples=" << buffer_samples_;
      ++log_count;
    }
    return frames;
  }

  void OnError(ErrorType) override {}

 private:
  AudioParameters params_;
  RAW_PTR_EXCLUSION void* mapped_ = nullptr;
  size_t mapped_size_ = 0;
  size_t buffer_samples_ = 0;
  int fd_ = -1;
};

bool AsmodeusSharedMemoryExists() {
  std::string path = GetAsmodeusAudioInPath();
  if (path.empty()) return false;
  struct stat st;
  return stat(path.c_str(), &st) == 0 && st.st_size > static_cast<off_t>(kShmHeaderSize);
}

}  // namespace

std::unique_ptr<AudioSourceCallback> FakeAudioInputStream::ChooseSource() {
  DCHECK(capture_thread_->task_runner()->BelongsToCurrentThread());

  // Asmodeus: if shared memory audio file exists, read from it
  std::string asmodeus_path = GetAsmodeusAudioInPath();
  if (!asmodeus_path.empty() && AsmodeusSharedMemoryExists()) {
    LOG(WARNING) << "[Asmodeus] Using shared memory audio source: " << asmodeus_path;
    return std::make_unique<SharedMemorySource>(params_, asmodeus_path);
  }

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kUseFileForFakeAudioCapture)) {
    base::CommandLine::StringType switch_value =
        base::CommandLine::ForCurrentProcess()->GetSwitchValueNative(
            switches::kUseFileForFakeAudioCapture);
    base::CommandLine::StringVector parameters =
        base::SplitString(switch_value, FILE_PATH_LITERAL("%"),
                          base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    CHECK(parameters.size() > 0) << "You must pass <file>[%noloop] to  --"
                                 << switches::kUseFileForFakeAudioCapture
                                 << ".";
    base::FilePath path_to_wav_file = base::FilePath(parameters[0]);
    bool looping = true;
    if (parameters.size() == 2) {
      CHECK(parameters[1] == FILE_PATH_LITERAL("noloop"))
          << "Unknown parameter " << parameters[1] << " to "
          << switches::kUseFileForFakeAudioCapture << ".";
      looping = false;
    }
    return std::make_unique<FileSource>(params_, path_to_wav_file, looping);
  }
  return std::make_unique<BeepingSource>(params_);
}

void FakeAudioInputStream::BeepOnce() {
  BeepingSource::BeepOnce();
}

void FakeAudioInputStream::SetGlobalMutedState(bool is_muted) {
  g_fake_input_streams_are_muted.store(is_muted, std::memory_order_relaxed);
}

}  // namespace media
