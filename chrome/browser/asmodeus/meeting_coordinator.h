// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_MEETING_COORDINATOR_H_
#define CHROME_BROWSER_ASMODEUS_MEETING_COORDINATOR_H_

#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/process/process.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "base/time/time.h"
#include "base/values.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"

namespace asmodeus {

class ControlClient;
class MeetingBackend;

// Manages a meeting: spawns agents, controls them, composites display, records.
class MeetingCoordinator {
 public:
  struct AgentEntry {
    AgentEntry();
    ~AgentEntry();
    AgentEntry(AgentEntry&&);
    AgentEntry& operator=(AgentEntry&&);

    std::string name;
    std::string display_name;
    std::string voice_model;
    std::string audio_shm_path;
    std::string video_shm_path;
    int control_port = 0;

    base::Process process;
    std::unique_ptr<ControlClient> control;

    // SHM readers (coordinator's own read positions)
    int audio_shm_fd = -1;
    RAW_PTR_EXCLUSION void* audio_shm_mapped = nullptr;
    size_t audio_shm_size = 0;
    size_t audio_read_pos = 0;

    int video_shm_fd = -1;
    RAW_PTR_EXCLUSION void* video_shm_mapped = nullptr;
    size_t video_shm_size = 0;

    bool speaking = false;
    double rms = 0.0;

    // Monitoring data (updated via OnAgentEvent on main thread)
    int heard_count = 0;
    int speech_count = 0;
    int barge_in_count = 0;
    std::string last_heard;
    std::string last_spoken;
    double last_speech_duration_ms = 0;
    int speaking_frames = 0;  // frames where rms > threshold during recording
  };

  struct TranscriptEntry {
    TranscriptEntry();
    TranscriptEntry(std::string speaker, std::string text,
                    std::string type, double timestamp_ms);
    ~TranscriptEntry();
    TranscriptEntry(TranscriptEntry&&);
    TranscriptEntry& operator=(TranscriptEntry&&);

    std::string speaker;
    std::string text;
    std::string type;      // "spoke", "heard", "barge_in"
    double timestamp_ms = 0;
  };

  MeetingCoordinator();
  ~MeetingCoordinator();

  MeetingCoordinator(const MeetingCoordinator&) = delete;
  MeetingCoordinator& operator=(const MeetingCoordinator&) = delete;

  // Lifecycle
  bool Start(const std::string& meeting_name,
             const std::string& agent_binary_path);
  // Start with an external backend (for browser-based platforms)
  bool StartWithBackend(std::unique_ptr<MeetingBackend> backend,
                        const std::string& agent_binary_path);
  void Stop();

  // Agents
  bool AddAgent(const std::string& name,
                const std::string& voice_model,
                const std::string& display_name);
  void RemoveAgent(const std::string& name);

  // Speech (async — sends to agent, returns immediately)
  bool Speak(const std::string& name, const std::string& text);

  // Display
  SkBitmap RenderFrame();

  // Recording
  bool StartRecording(const std::string& output_dir);
  bool StopRecording(int* out_frames, int64_t* out_samples,
                     double* out_peak_rms, std::string* out_mp4_path);

  // State
  const std::map<std::string, AgentEntry>& agents() const { return agents_; }
  bool is_recording() const { return recording_; }
  const std::vector<TranscriptEntry>& transcript() const { return transcript_; }

  // Event callback — called on main thread for each agent event.
  // Signature: (agent_name, event_type, event_data)
  using EventCallback = std::function<void(const std::string&,
                                            const std::string&,
                                            const base::Value&)>;
  void SetEventCallback(EventCallback cb) { event_callback_ = std::move(cb); }
  int signaling_port() const;

  // Handle agent event (called on main thread via PostTask)
  void OnAgentEvent(const std::string& name, base::Value event);

 private:
  void OnRecordTick();
  size_t ReadAgentAudio(AgentEntry& agent, float* output, size_t max_samples);
  SkBitmap ReadAgentVideo(const AgentEntry& agent);
  void OpenAgentShm(AgentEntry& agent);
  void CloseAgentShm(AgentEntry& agent);

  struct GridLayout {
    int cols, rows, tile_w, tile_h;
    SkRect GetTileRect(int index, int offset_y) const;
  };
  GridLayout ComputeGridLayout(int count, int width, int height);
  void DrawHeader(SkCanvas* canvas);
  void DrawNameBar(SkCanvas* canvas, const SkRect& rect,
                   const std::string& name, double rms);

  std::string meeting_name_;
  std::string agent_binary_path_;
  int next_control_port_ = 9620;

  std::unique_ptr<MeetingBackend> backend_;
  std::map<std::string, AgentEntry> agents_;

  // Recording
  bool recording_ = false;
  std::string output_dir_;
  std::ofstream wav_file_;
  int frame_count_ = 0;
  int64_t total_samples_ = 0;
  double peak_rms_ = 0;
  std::vector<int64_t> frame_timestamps_ms_;
  base::RepeatingTimer record_timer_;

  base::TimeTicks start_time_;
  scoped_refptr<base::SequencedTaskRunner> main_runner_;
  std::vector<TranscriptEntry> transcript_;
  EventCallback event_callback_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_MEETING_COORDINATOR_H_
