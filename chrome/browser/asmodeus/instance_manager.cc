// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/instance_manager.h"

#include <signal.h>
#include <sys/stat.h>

#include "base/command_line.h"
#include "base/logging.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"

namespace asmodeus {

InstanceInfo::InstanceInfo() = default;
InstanceInfo::~InstanceInfo() = default;
InstanceInfo::InstanceInfo(const InstanceInfo&) = default;
InstanceInfo& InstanceInfo::operator=(const InstanceInfo&) = default;
InstanceInfo::InstanceInfo(InstanceInfo&&) = default;
InstanceInfo& InstanceInfo::operator=(InstanceInfo&&) = default;

InstanceManager::InstanceManager(const std::string& chrome_path)
    : chrome_path_(chrome_path) {}

InstanceManager::~InstanceManager() {
  StopAll();
}

int InstanceManager::AllocatePort() {
  // Find first available port starting from 9300
  for (auto& [port, in_use] : port_pool_) {
    if (!in_use) {
      in_use = true;
      return port;
    }
  }
  // No freed ports, allocate new one
  int port = next_port_++;
  port_pool_[port] = true;
  return port;
}

void InstanceManager::ReleasePort(int port) {
  auto it = port_pool_.find(port);
  if (it != port_pool_.end()) {
    it->second = false;
  }
}

int InstanceManager::Launch(const std::string& agent_name,
                              const std::string& profile_path,
                              int preferred_port) {
  if (instances_.count(agent_name)) {
    LOG(WARNING) << "[Asmodeus] Instance already exists: " << agent_name;
    return instances_[agent_name].cdp_port;
  }

  int port = preferred_port > 0 ? preferred_port : AllocatePort();
  if (preferred_port > 0) {
    port_pool_[port] = true;
  }

  // Ensure profile directory exists
  mkdir(profile_path.c_str(), 0755);

  // Build audio SHM paths
  const char* home = getenv("HOME");
  std::string media_dir = home ? std::string(home) + "/.asmodeus" : "/tmp";
  std::string audio_in = media_dir + "/audio-in-" + agent_name + ".shm";
  std::string audio_out = media_dir + "/audio-out-" + agent_name + ".shm";

  // Build command line
  std::vector<std::string> args = {
      chrome_path_,
      "--remote-debugging-port=" + base::NumberToString(port),
      "--user-data-dir=" + profile_path,
      "--no-first-run",
      "--disable-session-crashed-bubble",
      "--noerrdialogs",
      "--enable-logging=stderr",
      "--v=0",
      "--autoplay-policy=no-user-gesture-required",
      "--use-fake-ui-for-media-stream",
      "--asmodeus-device=" + agent_name,
      "about:blank",
  };

  base::LaunchOptions options;
  options.disclaim_responsibility = true;

  base::Process process = base::LaunchProcess(args, options);
  if (!process.IsValid()) {
    LOG(ERROR) << "[Asmodeus] Failed to launch Chrome for " << agent_name;
    ReleasePort(port);
    return -1;
  }

  InstanceInfo info;
  info.agent_name = agent_name;
  info.profile_path = profile_path;
  info.cdp_port = port;
  info.status = InstanceStatus::kRunning;
  info.pid = process.Pid();
  info.audio_in_shm = audio_in;
  info.audio_out_shm = audio_out;

  instances_[agent_name] = std::move(info);

  LOG(INFO) << "[Asmodeus] Launched Chrome for " << agent_name
            << " pid=" << process.Pid()
            << " port=" << port
            << " profile=" << profile_path;

  // Process handle is intentionally released — we track by PID.
  // The process continues running independently.
  [[maybe_unused]] auto handle = process.Release();
  return port;
}

void InstanceManager::Stop(const std::string& agent_name) {
  auto it = instances_.find(agent_name);
  if (it == instances_.end()) return;

  auto& info = it->second;
  if (info.pid > 0) {
    kill(static_cast<pid_t>(info.pid), SIGTERM);
    LOG(INFO) << "[Asmodeus] Stopped Chrome for " << agent_name
              << " pid=" << info.pid;
  }

  ReleasePort(info.cdp_port);
  instances_.erase(it);
}

void InstanceManager::StopAll() {
  // Copy keys first (Stop modifies the map)
  std::vector<std::string> names;
  for (const auto& [name, _] : instances_) {
    names.push_back(name);
  }
  for (const auto& name : names) {
    Stop(name);
  }
}

const InstanceInfo* InstanceManager::Get(
    const std::string& agent_name) const {
  auto it = instances_.find(agent_name);
  return (it != instances_.end()) ? &it->second : nullptr;
}

bool InstanceManager::IsRunning(const std::string& agent_name) const {
  auto it = instances_.find(agent_name);
  if (it == instances_.end()) return false;
  // Check if process is still alive
  return kill(static_cast<pid_t>(it->second.pid), 0) == 0;
}

std::vector<InstanceInfo> InstanceManager::List() const {
  std::vector<InstanceInfo> result;
  for (const auto& [_, info] : instances_) {
    result.push_back(info);
  }
  return result;
}

}  // namespace asmodeus
