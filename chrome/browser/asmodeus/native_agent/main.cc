// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Standalone native agent binary for Asmodeus meetings.

#include <csignal>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/message_loop/message_pump_type.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "chrome/browser/asmodeus/native_agent/native_agent.h"

namespace {
asmodeus::NativeAgent* g_agent = nullptr;
void SignalHandler(int) {
  if (g_agent) g_agent->Shutdown();
}
}  // namespace

int main(int argc, char* argv[]) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  const auto& cmd = *base::CommandLine::ForCurrentProcess();

  asmodeus::NativeAgent::Config config;
  config.name = cmd.GetSwitchValueASCII("name");
  if (config.name.empty()) {
    fprintf(stderr,
        "Usage: asmodeus_agent --name=alice [--voice-model=...] "
        "[--control-port=9600] [--signaling-host=... --signaling-port=...] "
        "[--speak=\"text\"]\n");
    return 1;
  }

  config.display_name = cmd.GetSwitchValueASCII("display-name");
  if (config.display_name.empty()) {
    config.display_name = config.name;
    config.display_name[0] = toupper(config.display_name[0]);
  }

  std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
  config.voice_model = cmd.GetSwitchValueASCII("voice-model");
  if (config.voice_model.empty())
    config.voice_model = home + "/.asmodeus/voices/en_US-kristin-medium.onnx";
  config.piper_path = cmd.GetSwitchValueASCII("piper-path");
  if (config.piper_path.empty())
    config.piper_path = home + "/.asmodeus/venv/bin/piper";

  config.audio_shm_path = cmd.GetSwitchValueASCII("audio-shm");
  if (config.audio_shm_path.empty())
    config.audio_shm_path = home + "/.asmodeus/audio-in-" + config.name + ".shm";
  config.audio_out_shm_path = cmd.GetSwitchValueASCII("audio-out-shm");
  if (config.audio_out_shm_path.empty())
    config.audio_out_shm_path = home + "/.asmodeus/audio-out-" + config.name + ".shm";
  config.video_shm_path = cmd.GetSwitchValueASCII("video-shm");
  if (config.video_shm_path.empty())
    config.video_shm_path = home + "/.asmodeus/video-in-" + config.name + ".shm";

  if (cmd.HasSwitch("control-port"))
    config.control_port = std::stoi(cmd.GetSwitchValueASCII("control-port"));
  config.signaling_host = cmd.GetSwitchValueASCII("signaling-host");
  if (cmd.HasSwitch("signaling-port"))
    config.signaling_port = std::stoi(cmd.GetSwitchValueASCII("signaling-port"));

  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);
  logging::SetMinLogLevel(logging::LOGGING_INFO);

  // Create main thread executor (provides SequencedTaskRunner for the agent)
  base::SingleThreadTaskExecutor main_executor(base::MessagePumpType::DEFAULT);

  // Initialize thread pool (for non-blocking speak)
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("agent");

  asmodeus::NativeAgent agent(config);
  g_agent = &agent;

  if (!agent.Init()) {
    LOG(ERROR) << "Agent init failed";
    return 1;
  }

  // --speak flag: speak and exit
  std::string speak_text = cmd.GetSwitchValueASCII("speak");
  if (!speak_text.empty()) {
    agent.SpeakSync(speak_text);
    base::ThreadPoolInstance::Get()->Shutdown();
    return 0;
  }

  // Normal mode: run until shutdown (uses RunLoop internally)
  agent.Run();

  base::ThreadPoolInstance::Get()->Shutdown();
  LOG(INFO) << "[" << config.name << "] Agent stopped.";
  return 0;
}
