// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_INSTANCE_MANAGER_H_
#define CHROME_BROWSER_ASMODEUS_INSTANCE_MANAGER_H_

#include <map>
#include <string>
#include <vector>

#include "base/process/process.h"

namespace asmodeus {

// Status of a Chrome instance.
enum class InstanceStatus {
  kRunning,
  kStopped,
  kCrashed,
  kUnknown,
};

// Info about a running Chrome instance.
struct InstanceInfo {
  InstanceInfo();
  ~InstanceInfo();
  InstanceInfo(const InstanceInfo&);
  InstanceInfo& operator=(const InstanceInfo&);
  InstanceInfo(InstanceInfo&&);
  InstanceInfo& operator=(InstanceInfo&&);

  std::string agent_name;
  std::string profile_path;
  int cdp_port = 0;
  InstanceStatus status = InstanceStatus::kUnknown;
  base::ProcessId pid = 0;
  // Audio SHM paths
  std::string audio_in_shm;
  std::string audio_out_shm;
};

// Manages multiple Chrome instances, one per agent.
// Each instance gets its own profile directory, CDP port, and audio SHM files.
//
// Usage:
//   InstanceManager mgr("/path/to/chromium");
//   int port = mgr.Launch("ultron", "~/.asmodeus/profiles/ultron");
//   // ... use CDP at localhost:port ...
//   mgr.Stop("ultron");
class InstanceManager {
 public:
  // chrome_path: path to the Chromium binary
  explicit InstanceManager(const std::string& chrome_path);
  ~InstanceManager();

  // Launch a new Chrome instance for the given agent.
  // Returns the CDP port, or -1 on failure.
  int Launch(const std::string& agent_name,
             const std::string& profile_path,
             int preferred_port = 0);

  // Stop an instance.
  void Stop(const std::string& agent_name);

  // Stop all instances.
  void StopAll();

  // Get instance info. Returns nullptr if not found.
  const InstanceInfo* Get(const std::string& agent_name) const;

  // Check if an instance exists and is running.
  bool IsRunning(const std::string& agent_name) const;

  // List all instances.
  std::vector<InstanceInfo> List() const;

  // Allocate a unique CDP port (9300+).
  int AllocatePort();

  // Release a port back to the pool.
  void ReleasePort(int port);

 private:
  std::string chrome_path_;
  std::map<std::string, InstanceInfo> instances_;
  std::map<int, bool> port_pool_;  // port → in_use
  int next_port_ = 9300;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_INSTANCE_MANAGER_H_
