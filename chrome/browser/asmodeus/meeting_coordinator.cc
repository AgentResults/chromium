// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/meeting_coordinator.h"

#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "base/compiler_specific.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/asmodeus/control_client.h"
#include "chrome/browser/asmodeus/meeting_backend.h"
#include "chrome/browser/asmodeus/native_meeting_backend.h"
#include "chrome/browser/asmodeus/native_agent/video_shm.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"
#include "third_party/libyuv/include/libyuv.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/encode/SkJpegEncoder.h"
#include "third_party/skia/include/core/SkStream.h"

namespace asmodeus {

MeetingCoordinator::AgentEntry::AgentEntry() = default;
MeetingCoordinator::AgentEntry::~AgentEntry() = default;
MeetingCoordinator::AgentEntry::AgentEntry(AgentEntry&&) = default;
MeetingCoordinator::AgentEntry& MeetingCoordinator::AgentEntry::operator=(AgentEntry&&) = default;

MeetingCoordinator::TranscriptEntry::TranscriptEntry() = default;
MeetingCoordinator::TranscriptEntry::TranscriptEntry(
    std::string s, std::string t, std::string ty, double ts)
    : speaker(std::move(s)), text(std::move(t)),
      type(std::move(ty)), timestamp_ms(ts) {}
MeetingCoordinator::TranscriptEntry::~TranscriptEntry() = default;
MeetingCoordinator::TranscriptEntry::TranscriptEntry(TranscriptEntry&&) = default;
MeetingCoordinator::TranscriptEntry& MeetingCoordinator::TranscriptEntry::operator=(TranscriptEntry&&) = default;

MeetingCoordinator::MeetingCoordinator()
    : main_runner_(base::SequencedTaskRunner::GetCurrentDefault()) {}

MeetingCoordinator::~MeetingCoordinator() {
  Stop();
}

bool MeetingCoordinator::Start(const std::string& meeting_name,
                                const std::string& agent_binary_path) {
  meeting_name_ = meeting_name;
  agent_binary_path_ = agent_binary_path;
  start_time_ = base::TimeTicks::Now();

  backend_ = std::make_unique<NativeMeetingBackend>();
  if (!backend_->Start(meeting_name)) {
    LOG(ERROR) << "Failed to start meeting backend";
    return false;
  }

  LOG(INFO) << "Meeting coordinator started: " << meeting_name;
  return true;
}

bool MeetingCoordinator::StartWithBackend(
    std::unique_ptr<MeetingBackend> backend,
    const std::string& agent_binary_path) {
  agent_binary_path_ = agent_binary_path;
  start_time_ = base::TimeTicks::Now();
  backend_ = std::move(backend);
  meeting_name_ = "platform-meeting";
  LOG(INFO) << "Meeting coordinator started with external backend";
  return true;
}

void MeetingCoordinator::Stop() {
  if (recording_) {
    int f; int64_t s; double r; std::string p;
    StopRecording(&f, &s, &r, &p);
  }

  // Remove all agents
  std::vector<std::string> names;
  for (auto& [n, _] : agents_) names.push_back(n);
  for (auto& n : names) RemoveAgent(n);

  if (backend_) {
    backend_->Stop();
    backend_.reset();
  }
  LOG(INFO) << "Meeting coordinator stopped";
}

void MeetingCoordinator::OnAgentEvent(const std::string& name,
                                       base::Value event) {
  auto it = agents_.find(name);
  if (it == agents_.end()) return;
  auto& agent = it->second;

  if (!event.is_dict()) return;
  const auto& dict = event.GetDict();
  const std::string* evt = dict.FindString("event");
  if (!evt) return;

  double ts = (base::TimeTicks::Now() - start_time_).InMillisecondsF();

  if (*evt == "speech_started") {
    agent.speaking = true;
    agent.speech_count++;
    const std::string* text = dict.FindString("text");
    if (text) {
      agent.last_spoken = *text;
      transcript_.push_back({name, *text, "spoke", ts});
    }
  } else if (*evt == "speech_ended") {
    agent.speaking = false;
    auto dur = dict.FindDouble("durationMs");
    if (dur) agent.last_speech_duration_ms = *dur;
  } else if (*evt == "heard") {
    agent.heard_count++;
    const std::string* text = dict.FindString("text");
    if (text) {
      agent.last_heard = *text;
      transcript_.push_back({name, *text, "heard", ts});
    }
  } else if (*evt == "barge_in") {
    agent.barge_in_count++;
    transcript_.push_back({name, "", "barge_in", ts});
  } else if (*evt == "error") {
    // just forward
  }

  // Forward to CDP handler
  if (event_callback_) {
    event_callback_(name, *evt, event);
  }
}

bool MeetingCoordinator::AddAgent(const std::string& name,
                                   const std::string& voice_model,
                                   const std::string& display_name) {
  if (agents_.count(name)) {
    LOG(ERROR) << "Agent already exists: " << name;
    return false;
  }

  AgentEntry entry;
  entry.name = name;
  entry.display_name = display_name.empty() ? name : display_name;
  if (entry.display_name[0] >= 'a' && entry.display_name[0] <= 'z')
    entry.display_name[0] -= 32;
  entry.voice_model = voice_model;
  entry.control_port = next_control_port_++;

  std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
  std::string media_dir = home + "/.asmodeus";
  mkdir(media_dir.c_str(), 0755);  // raw mkdir — no DCHECK on UI thread
  entry.audio_shm_path = media_dir + "/audio-in-" + name + ".shm";
  entry.video_shm_path = media_dir + "/video-in-" + name + ".shm";

  // For native meetings: don't pre-create SHM — the agent Creates them.
  // For browser platform: BrowserPlatform::ConnectAgent pre-creates them.

  // Spawn agent process
  std::vector<std::string> args;
  args.push_back(agent_binary_path_);
  args.push_back("--name=" + name);
  args.push_back("--voice-model=" + voice_model);
  args.push_back("--control-port=" + base::NumberToString(entry.control_port));

  if (backend_ && !backend_->GetSignalingHost().empty()) {
    args.push_back("--signaling-host=" + backend_->GetSignalingHost());
    args.push_back("--signaling-port=" + base::NumberToString(backend_->GetSignalingPort()));
  }

  base::LaunchOptions options;
  entry.process = base::LaunchProcess(args, options);
  if (!entry.process.IsValid()) {
    LOG(ERROR) << "Failed to spawn agent: " << name;
    return false;
  }
  LOG(INFO) << "Agent spawned: " << name << " pid=" << entry.process.Pid()
            << " control=" << entry.control_port;

  // Wait for agent to be ready (poll health endpoint)
  bool ready = false;
  for (int i = 0; i < 60; i++) {
    // Try connecting to the health endpoint
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(entry.control_port);
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
        close(fd);
        ready = true;
        break;
      }
      close(fd);
    }
    usleep(500000);  // 500ms
  }
  if (!ready) {
    LOG(ERROR) << "Agent not ready after 30s: " << name;
    entry.process.Terminate(0, false);
    return false;
  }

  // Connect control client
  entry.control = std::make_unique<ControlClient>();
  if (!entry.control->Connect("127.0.0.1", entry.control_port)) {
    LOG(ERROR) << "Failed to connect control client for: " << name;
    entry.process.Terminate(0, false);
    return false;
  }

  // Set up event handler — PostTask to main thread for thread safety
  entry.control->on_event = [this, name](const std::string& json) {
    auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
    if (!parsed) return;
    main_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&MeetingCoordinator::OnAgentEvent,
                       base::Unretained(this), name, std::move(*parsed)));
  };

  // Open shm for reading
  OpenAgentShm(entry);

  // Connect to backend
  backend_->ConnectAgent(name, entry.audio_shm_path, entry.video_shm_path);

  agents_[name] = std::move(entry);
  LOG(INFO) << "Agent added: " << name;
  return true;
}

void MeetingCoordinator::RemoveAgent(const std::string& name) {
  auto it = agents_.find(name);
  if (it == agents_.end()) return;

  auto& entry = it->second;

  // Disconnect from backend
  if (backend_) backend_->DisconnectAgent(name);

  LOG(INFO) << "RemoveAgent: killing process for " << name;
  // Kill agent process first (this breaks the control socket)
  if (entry.process.IsValid()) {
    entry.process.Terminate(0, false);
  }
  LOG(INFO) << "RemoveAgent: disconnecting control for " << name;
  // Then disconnect control (socket is already broken, thread will exit)
  if (entry.control) {
    entry.control->Disconnect();
  }
  LOG(INFO) << "RemoveAgent: done for " << name;

  CloseAgentShm(entry);
  agents_.erase(it);
  LOG(INFO) << "Agent removed: " << name;
}

bool MeetingCoordinator::Speak(const std::string& name, const std::string& text) {
  auto it = agents_.find(name);
  if (it == agents_.end()) return false;

  std::string cmd = "{\"id\":1,\"command\":\"speak\",\"text\":\"";
  // Simple JSON escape for text
  for (char c : text) {
    if (c == '"') cmd += "\\\"";
    else if (c == '\\') cmd += "\\\\";
    else if (c == '\n') cmd += "\\n";
    else cmd += c;
  }
  cmd += "\"}";

  std::string response = it->second.control->SendCommand(cmd);
  return !response.empty();
}

int MeetingCoordinator::signaling_port() const {
  return backend_ ? backend_->GetSignalingPort() : 0;
}

// ── SHM Reading ──

void MeetingCoordinator::OpenAgentShm(AgentEntry& agent) {
  // Audio shm
  agent.audio_shm_fd = open(agent.audio_shm_path.c_str(), O_RDONLY);
  if (agent.audio_shm_fd >= 0) {
    off_t size = lseek(agent.audio_shm_fd, 0, SEEK_END);
    lseek(agent.audio_shm_fd, 0, SEEK_SET);
    agent.audio_shm_size = size;
    agent.audio_shm_mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED,
                                   agent.audio_shm_fd, 0);
    if (agent.audio_shm_mapped != MAP_FAILED) {
      // Initialize read pos to current write pos (skip existing data)
      auto* header = reinterpret_cast<const AudioRingBuffer::Header*>(
          agent.audio_shm_mapped);
      agent.audio_read_pos = header->write_pos.load();
    } else {
      agent.audio_shm_mapped = nullptr;
    }
  }

  // Video shm
  agent.video_shm_fd = open(agent.video_shm_path.c_str(), O_RDONLY);
  if (agent.video_shm_fd >= 0) {
    off_t size = lseek(agent.video_shm_fd, 0, SEEK_END);
    lseek(agent.video_shm_fd, 0, SEEK_SET);
    agent.video_shm_size = size;
    agent.video_shm_mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED,
                                   agent.video_shm_fd, 0);
    if (agent.video_shm_mapped == MAP_FAILED) {
      agent.video_shm_mapped = nullptr;
    }
  }
}

void MeetingCoordinator::CloseAgentShm(AgentEntry& agent) {
  if (agent.audio_shm_mapped) {
    munmap(agent.audio_shm_mapped, agent.audio_shm_size);
    close(agent.audio_shm_fd);
    agent.audio_shm_mapped = nullptr;
  }
  if (agent.video_shm_mapped) {
    munmap(agent.video_shm_mapped, agent.video_shm_size);
    close(agent.video_shm_fd);
    agent.video_shm_mapped = nullptr;
  }
}

size_t MeetingCoordinator::ReadAgentAudio(AgentEntry& agent, float* output,
                                           size_t max_samples) {
  if (!agent.audio_shm_mapped) return 0;

  auto* header = reinterpret_cast<const AudioRingBuffer::Header*>(
      agent.audio_shm_mapped);
  const float* buffer = reinterpret_cast<const float*>(header + 1);
  size_t capacity = (agent.audio_shm_size - sizeof(*header)) / sizeof(float);
  if (capacity == 0) return 0;

  // write_pos is monotonic (never wraps). Use simple subtraction.
  uint32_t write_pos = header->write_pos.load(std::memory_order_acquire);
  size_t available = write_pos - agent.audio_read_pos;
  if (available > capacity) {
    // Reader fell too far behind — skip to near the write head.
    agent.audio_read_pos = write_pos - capacity / 2;
    available = write_pos - agent.audio_read_pos;
  }
  size_t to_read = std::min(available, max_samples);

  for (size_t i = 0; i < to_read; i++) {
    output[i] = buffer[(agent.audio_read_pos + i) % capacity];
  }
  agent.audio_read_pos += to_read;  // Monotonic — don't wrap
  return to_read;
}

SkBitmap MeetingCoordinator::ReadAgentVideo(const AgentEntry& agent) {
  SkBitmap bitmap;
  if (!agent.video_shm_mapped) {
    bitmap.allocN32Pixels(640, 480);
    bitmap.eraseColor(SkColorSetRGB(0x0f, 0x34, 0x60));
    return bitmap;
  }

  auto* header = reinterpret_cast<const VideoShmHeader*>(agent.video_shm_mapped);
  int w = header->width, h = header->height;
  uint32_t frame_size = header->frame_size;
  uint32_t current = header->current_buffer.load();
  const uint8_t* base = reinterpret_cast<const uint8_t*>(agent.video_shm_mapped) +
                         sizeof(VideoShmHeader);
  const uint8_t* nv12 = base + current * frame_size;

  bitmap.allocN32Pixels(w, h);
  libyuv::NV12ToARGB(nv12, w, nv12 + w * h, w,
                      reinterpret_cast<uint8_t*>(bitmap.getPixels()), w * 4,
                      w, h);
  return bitmap;
}

// ── Compositor ──

MeetingCoordinator::GridLayout MeetingCoordinator::ComputeGridLayout(
    int count, int width, int height) {
  GridLayout layout;
  if (count <= 1) { layout.cols = 1; layout.rows = 1; }
  else if (count <= 2) { layout.cols = 2; layout.rows = 1; }
  else if (count <= 4) { layout.cols = 2; layout.rows = 2; }
  else if (count <= 6) { layout.cols = 3; layout.rows = 2; }
  else { layout.cols = 3; layout.rows = 3; }
  layout.tile_w = (width - (layout.cols + 1) * 8) / layout.cols;
  layout.tile_h = (height - (layout.rows + 1) * 8) / layout.rows;
  return layout;
}

SkRect MeetingCoordinator::GridLayout::GetTileRect(int index, int offset_y) const {
  int col = index % cols;
  int row = index / cols;
  float x = 8 + col * (tile_w + 8);
  float y = offset_y + 8 + row * (tile_h + 8);
  return SkRect::MakeXYWH(x, y, tile_w, tile_h);
}

void MeetingCoordinator::DrawHeader(SkCanvas* canvas) {
  // Meeting name
  SkFont font;
  font.setSize(16);
  SkPaint paint;
  paint.setColor(SK_ColorWHITE);
  paint.setAntiAlias(true);
  canvas->drawString(meeting_name_.c_str(), 20, 32, font, paint);

  // Timer
  int elapsed = static_cast<int>(
      (base::TimeTicks::Now() - start_time_).InSecondsF());
  int m = elapsed / 60, s = elapsed % 60;
  std::string timer = base::StringPrintf("%02d:%02d", m, s);
  SkPaint timer_paint;
  timer_paint.setColor(SkColorSetARGB(255, 160, 160, 176));
  timer_paint.setAntiAlias(true);
  canvas->drawString(timer.c_str(), 580, 32, font, timer_paint);

  // Participant count
  std::string count = base::NumberToString(agents_.size()) + " participants";
  SkPaint count_paint;
  count_paint.setColor(SkColorSetRGB(0x4a, 0xde, 0x80));
  count_paint.setAntiAlias(true);
  canvas->drawString(count.c_str(), 1100, 32, font, count_paint);
}

void MeetingCoordinator::DrawNameBar(SkCanvas* canvas, const SkRect& rect,
                                      const std::string& name, double rms) {
  // Gradient background at bottom of tile
  SkPaint bg;
  bg.setColor(SkColorSetARGB(180, 0, 0, 0));
  SkRect bar = SkRect::MakeXYWH(rect.fLeft, rect.fBottom - 30,
                                  rect.width(), 30);
  canvas->drawRect(bar, bg);

  // Name text
  SkFont font;
  font.setSize(14);
  SkPaint text;
  text.setColor(SK_ColorWHITE);
  text.setAntiAlias(true);
  canvas->drawString(name.c_str(), rect.fLeft + 12, rect.fBottom - 10,
                      font, text);
}

SkBitmap MeetingCoordinator::RenderFrame() {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(1280, 720);
  SkCanvas canvas(bitmap);
  canvas.clear(SkColorSetRGB(0x1a, 0x1a, 0x2e));

  DrawHeader(&canvas);

  auto layout = ComputeGridLayout(agents_.size(), 1280, 672);
  int idx = 0;
  for (auto& [name, agent] : agents_) {
    SkRect rect = layout.GetTileRect(idx++, 48);

    // Tile background
    SkPaint tile_bg;
    tile_bg.setColor(SkColorSetRGB(0x0f, 0x34, 0x60));
    tile_bg.setAntiAlias(true);
    SkRRect rrect = SkRRect::MakeRectXY(rect, 12, 12);
    canvas.drawRRect(rrect, tile_bg);

    // Agent's avatar from video shm
    SkBitmap avatar = ReadAgentVideo(agent);
    canvas.save();
    canvas.clipRRect(rrect, true);
    canvas.drawImageRect(avatar.asImage(), rect,
                          SkSamplingOptions(SkFilterMode::kLinear));
    canvas.restore();

    // Speaking border
    if (agent.speaking) {
      SkPaint border;
      border.setColor(SkColorSetRGB(0x4a, 0xde, 0x80));
      border.setStyle(SkPaint::kStroke_Style);
      border.setStrokeWidth(3);
      border.setAntiAlias(true);
      canvas.drawRRect(rrect, border);
    }

    DrawNameBar(&canvas, rect, agent.display_name, agent.rms);
  }

  return bitmap;
}

// ── Recording ──

bool MeetingCoordinator::StartRecording(const std::string& output_dir) {
  if (recording_) return false;
  output_dir_ = output_dir;
  frame_count_ = 0;
  total_samples_ = 0;
  peak_rms_ = 0;
  frame_timestamps_ms_.clear();

  // Create directories
  std::string frames_dir = output_dir + "/frames";
  system(("mkdir -p " + frames_dir).c_str());

  // Open WAV file
  std::string wav_path = output_dir + "/audio.wav";
  wav_file_.open(wav_path, std::ios::binary);
  if (!wav_file_.is_open()) return false;

  // Write placeholder WAV header
  auto w16 = [this](uint16_t v) { wav_file_.write(reinterpret_cast<const char*>(&v), 2); };
  auto w32 = [this](uint32_t v) { wav_file_.write(reinterpret_cast<const char*>(&v), 4); };
  wav_file_.write("RIFF", 4); w32(0);
  wav_file_.write("WAVE", 4); wav_file_.write("fmt ", 4); w32(16);
  w16(1); w16(1); w32(48000); w32(96000); w16(2); w16(16);
  wav_file_.write("data", 4); w32(0);

  recording_ = true;
  record_timer_.Start(FROM_HERE, base::Milliseconds(100),
                       base::BindRepeating(&MeetingCoordinator::OnRecordTick,
                                           base::Unretained(this)));

  LOG(INFO) << "Recording started: " << output_dir;
  return true;
}

bool MeetingCoordinator::StopRecording(int* out_frames, int64_t* out_samples,
                                        double* out_peak_rms,
                                        std::string* out_mp4_path) {
  if (!recording_) return false;
  recording_ = false;
  record_timer_.Stop();

  // Finalize WAV
  auto pos = wav_file_.tellp();
  uint32_t data_size = static_cast<uint32_t>(pos) - 44;
  uint32_t file_size = static_cast<uint32_t>(pos) - 8;
  wav_file_.seekp(4);
  wav_file_.write(reinterpret_cast<const char*>(&file_size), 4);
  wav_file_.seekp(40);
  wav_file_.write(reinterpret_cast<const char*>(&data_size), 4);
  wav_file_.close();

  // Encode MP4 via ffmpeg
  std::string mp4_path = output_dir_ + "/meeting.mp4";
  if (frame_count_ > 5) {
    // Write concat file
    std::ofstream concat(output_dir_ + "/frames.txt");
    for (int i = 0; i < frame_count_; i++) {
      concat << "file 'frames/frame_" << base::StringPrintf("%05d", i) << ".jpg'\n";
      double dur = (i < frame_count_ - 1)
          ? std::max(0.033, (frame_timestamps_ms_[i+1] - frame_timestamps_ms_[i]) / 1000.0)
          : 0.100;
      concat << "duration " << dur << "\n";
    }
    concat.close();

    std::string cmd = "cd \"" + output_dir_ + "\" && ffmpeg -y -f concat -safe 0 -i frames.txt "
        "-i audio.wav -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\" "
        "-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p "
        "-c:a aac -b:a 192k -shortest meeting.mp4 2>/dev/null";
    system(cmd.c_str());
  }

  *out_frames = frame_count_;
  *out_samples = total_samples_;
  *out_peak_rms = peak_rms_;
  *out_mp4_path = mp4_path;

  LOG(INFO) << "Recording stopped: " << frame_count_ << " frames, "
            << total_samples_ << " samples";
  return true;
}

void MeetingCoordinator::OnRecordTick() {
  const int samples_per_tick = 4800;  // 100ms at 48kHz
  float mixed[4800] = {0};

  for (auto& [name, agent] : agents_) {
    float chunk[4800];
    size_t read = ReadAgentAudio(agent, chunk, samples_per_tick);
    for (size_t i = 0; i < read; i++) mixed[i] += chunk[i];

    // Speaking detection
    float rms = 0;
    for (size_t i = 0; i < read; i++) rms += chunk[i] * chunk[i];
    agent.rms = std::sqrt(rms / std::max(read, (size_t)1));
    bool was_speaking = agent.speaking;
    agent.speaking = agent.rms > 0.01;
    if (agent.speaking) agent.speaking_frames++;
    if (agent.speaking && !was_speaking) {
      LOG(INFO) << "Recording: " << name << " started speaking rms="
                << agent.rms << " read=" << read;
    }
  }

  // Clamp and write to WAV
  for (int i = 0; i < samples_per_tick; i++) {
    mixed[i] = std::clamp(mixed[i], -1.0f, 1.0f);
    int16_t s = static_cast<int16_t>(mixed[i] * 32767);
    wav_file_.write(reinterpret_cast<const char*>(&s), 2);
  }
  total_samples_ += samples_per_tick;

  // Track peak RMS
  float tick_rms = 0;
  for (int i = 0; i < samples_per_tick; i++) tick_rms += mixed[i] * mixed[i];
  tick_rms = std::sqrt(tick_rms / samples_per_tick);
  if (tick_rms > peak_rms_) peak_rms_ = tick_rms;

  // Composite video
  SkBitmap frame = RenderFrame();
  std::string frame_path = output_dir_ + "/frames/frame_" +
      base::StringPrintf("%05d", frame_count_) + ".jpg";
  SkFILEWStream stream(frame_path.c_str());
  if (stream.isValid()) {
    SkJpegEncoder::Encode(&stream, frame.pixmap(), {75});
  }

  frame_timestamps_ms_.push_back(
      (base::TimeTicks::Now() - start_time_).InMilliseconds());
  frame_count_++;
}

}  // namespace asmodeus
