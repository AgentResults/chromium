// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Standalone meeting coordinator binary.
// Runs native WebRTC meetings without Chrome.
//
// Usage:
//   asmodeus_meeting --agent-binary=/path/to/asmodeus_agent \
//     --agents="alice:en_US-amy-medium,bob:en_US-danny-low"
//
// Control via HTTP on --control-port (default 9630):
//   POST /api/addAgent     {"name":"carol","voiceModel":"en_US-hfc_female-medium"}
//   POST /api/removeAgent  {"name":"carol"}
//   POST /api/speak        {"name":"alice","text":"Hello everyone"}
//   GET  /api/state        -> meeting state JSON
//   GET  /api/transcript   -> transcript JSON
//   POST /api/stop         -> stop meeting

#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/message_loop/message_pump_type.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "base/values.h"
#include "chrome/browser/asmodeus/meeting_coordinator.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"

namespace {

asmodeus::MeetingCoordinator* g_coordinator = nullptr;
base::RunLoop* g_run_loop = nullptr;

void SignalHandler(int sig) {
  LOG(INFO) << "Received signal " << sig << ", shutting down...";
  if (g_run_loop) g_run_loop->Quit();
}

// Simple HTTP control server for the meeting.
class MeetingControlServer : public net::HttpServer::Delegate {
 public:
  MeetingControlServer(asmodeus::MeetingCoordinator* coordinator, int port)
      : coordinator_(coordinator) {
    auto socket =
        std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
    socket->ListenWithAddressAndPort("127.0.0.1", port, 10);
    server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
    LOG(INFO) << "Control server: http://127.0.0.1:" << port;
  }

  ~MeetingControlServer() override = default;

 private:
  void OnConnect(int id) override {}
  void OnClose(int id) override {}
  void OnWebSocketRequest(int id,
                          const net::HttpServerRequestInfo& info) override {
    server_->Close(id);
  }
  void OnWebSocketMessage(int id, std::string data) override {}

  void OnHttpRequest(int id,
                     const net::HttpServerRequestInfo& info) override {
    const std::string& path = info.path;

    if (path == "/api/state") {
      HandleGetState(id);
    } else if (path == "/api/transcript") {
      HandleGetTranscript(id);
    } else if (path == "/api/addAgent") {
      HandleAddAgent(id, info.data);
    } else if (path == "/api/removeAgent") {
      HandleRemoveAgent(id, info.data);
    } else if (path == "/api/speak") {
      HandleSpeak(id, info.data);
    } else if (path == "/api/startRecording") {
      HandleStartRecording(id, info.data);
    } else if (path == "/api/stopRecording") {
      HandleStopRecording(id);
    } else if (path == "/api/stop") {
      Reply(id, R"({"ok":true})");
      if (g_run_loop) g_run_loop->Quit();
    } else {
      Reply(id, R"({"error":"not found"})");
    }
  }

  void HandleGetState(int id) {
    std::string json = "[";
    bool first = true;
    for (const auto& [name, agent] : coordinator_->agents()) {
      if (!first) json += ",";
      first = false;
      json += base::StringPrintf(
          R"({"name":"%s","displayName":"%s","heardCount":%d,)"
          R"("speechCount":%d,"bargeInCount":%d,"speaking":%s,)"
          R"("speakingFrames":%d})",
          name.c_str(), agent.display_name.c_str(), agent.heard_count,
          agent.speech_count, agent.barge_in_count,
          agent.speaking ? "true" : "false", agent.speaking_frames);
    }
    json += "]";
    Reply(id, base::StringPrintf(
                  R"({"agents":%s,"recording":%s,"signalingPort":%d})",
                  json.c_str(),
                  coordinator_->is_recording() ? "true" : "false",
                  coordinator_->signaling_port()));
  }

  void HandleGetTranscript(int id) {
    std::string json = "[";
    bool first = true;
    for (const auto& e : coordinator_->transcript()) {
      if (!first) json += ",";
      first = false;
      // Simple JSON escape for text
      std::string escaped;
      for (char c : e.text) {
        if (c == '"')
          escaped += "\\\"";
        else if (c == '\\')
          escaped += "\\\\";
        else if (c == '\n')
          escaped += "\\n";
        else
          escaped += c;
      }
      json += base::StringPrintf(
          R"({"speaker":"%s","text":"%s","type":"%s","timestampMs":%.1f})",
          e.speaker.c_str(), escaped.c_str(), e.type.c_str(), e.timestamp_ms);
    }
    json += "]";
    Reply(id, base::StringPrintf(R"({"entries":%s})", json.c_str()));
  }

  void HandleAddAgent(int id, const std::string& body) {
    auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_dict()) {
      Reply(id, R"({"error":"invalid JSON"})");
      return;
    }
    auto& dict = parsed->GetDict();
    const std::string* name = dict.FindString("name");
    const std::string* voice = dict.FindString("voiceModel");
    const std::string* display = dict.FindString("displayName");
    if (!name || !voice) {
      Reply(id, R"({"error":"name and voiceModel required"})");
      return;
    }
    bool ok =
        coordinator_->AddAgent(*name, *voice, display ? *display : *name);
    Reply(id, ok ? R"({"ok":true})" : R"({"error":"failed to add agent"})");
  }

  void HandleRemoveAgent(int id, const std::string& body) {
    auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_dict()) {
      Reply(id, R"({"error":"invalid JSON"})");
      return;
    }
    const std::string* name = parsed->GetDict().FindString("name");
    if (!name) {
      Reply(id, R"({"error":"name required"})");
      return;
    }
    coordinator_->RemoveAgent(*name);
    Reply(id, R"({"ok":true})");
  }

  void HandleSpeak(int id, const std::string& body) {
    auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_dict()) {
      Reply(id, R"({"error":"invalid JSON"})");
      return;
    }
    auto& dict = parsed->GetDict();
    const std::string* name = dict.FindString("name");
    const std::string* text = dict.FindString("text");
    if (!name || !text) {
      Reply(id, R"({"error":"name and text required"})");
      return;
    }
    bool ok = coordinator_->Speak(*name, *text);
    Reply(id, ok ? R"({"queued":true})" : R"({"queued":false})");
  }

  void HandleStartRecording(int id, const std::string& body) {
    auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
    std::string output_dir;
    if (parsed && parsed->is_dict()) {
      const std::string* dir = parsed->GetDict().FindString("outputDir");
      if (dir) output_dir = *dir;
    }
    if (output_dir.empty()) {
      std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
      output_dir = home + "/.asmodeus/recordings/meeting";
    }
    bool ok = coordinator_->StartRecording(output_dir);
    Reply(id, ok ? R"({"started":true})" : R"({"started":false})");
  }

  void HandleStopRecording(int id) {
    int frames = 0;
    int64_t samples = 0;
    double peak_rms = 0;
    std::string mp4_path;
    bool ok = coordinator_->StopRecording(&frames, &samples, &peak_rms, &mp4_path);
    if (ok) {
      Reply(id, base::StringPrintf(
          R"({"frames":%d,"audioSamples":%)" PRId64
          R"(,"peakRms":%f,"mp4Path":"%s"})",
          frames, samples, peak_rms, mp4_path.c_str()));
    } else {
      Reply(id, R"({"error":"not recording"})");
    }
  }

  void Reply(int id, const std::string& json) {
    server_->Send200(id, json, "application/json",
                     TRAFFIC_ANNOTATION_FOR_TESTS);
  }

  raw_ptr<asmodeus::MeetingCoordinator> coordinator_;
  std::unique_ptr<net::HttpServer> server_;
};

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager exit_manager;
  base::CommandLine::Init(argc, argv);
  const auto& cmd = *base::CommandLine::ForCurrentProcess();

  logging::SetMinLogLevel(logging::LOGGING_INFO);

  std::string meeting_name = cmd.GetSwitchValueASCII("meeting-name");
  if (meeting_name.empty()) meeting_name = "Meeting";

  std::string agent_binary = cmd.GetSwitchValueASCII("agent-binary");
  if (agent_binary.empty()) {
    LOG(ERROR) << "Usage: asmodeus_meeting --agent-binary=PATH "
               << "[--meeting-name=NAME] "
               << "[--agents=name:voice,...] [--control-port=PORT]";
    return 1;
  }

  int control_port = 9630;
  std::string port_str = cmd.GetSwitchValueASCII("control-port");
  if (!port_str.empty())
    base::StringToInt(port_str, &control_port);

  std::string agents_str = cmd.GetSwitchValueASCII("agents");

  // Message loop needed for net::HttpServer and base::Thread.
  base::SingleThreadTaskExecutor main_task_executor(base::MessagePumpType::IO);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams(
      "asmodeus_meeting");

  // Create and start meeting coordinator.
  asmodeus::MeetingCoordinator coordinator;
  g_coordinator = &coordinator;

  if (!coordinator.Start(meeting_name, agent_binary)) {
    LOG(ERROR) << "Failed to start meeting";
    return 1;
  }

  LOG(INFO) << "Meeting '" << meeting_name << "' started"
            << " signaling=ws://127.0.0.1:" << coordinator.signaling_port();

  // Control HTTP server.
  MeetingControlServer control_server(&coordinator, control_port);

  // Log events.
  coordinator.SetEventCallback(
      [](const std::string& agent, const std::string& event,
         const base::Value& data) {
        LOG(INFO) << "[EVENT] " << agent << ": " << event;
      });

  // Add initial agents from --agents flag.
  if (!agents_str.empty()) {
    for (const auto& spec :
         base::SplitString(agents_str, ",", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      auto parts = base::SplitString(spec, ":", base::TRIM_WHITESPACE,
                                     base::SPLIT_WANT_NONEMPTY);
      if (parts.size() >= 2) {
        LOG(INFO) << "Adding agent: " << parts[0] << " voice=" << parts[1];
        coordinator.AddAgent(parts[0], parts[1], parts[0]);
      }
    }
  }

  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  LOG(INFO) << "Control: http://127.0.0.1:" << control_port << "/api/state";
  LOG(INFO) << "Press Ctrl+C to stop.";

  base::RunLoop run_loop;
  g_run_loop = &run_loop;
  run_loop.Run();

  LOG(INFO) << "Shutting down...";
  coordinator.Stop();
  base::ThreadPoolInstance::Get()->Shutdown();
  LOG(INFO) << "Done.";
  return 0;
}
