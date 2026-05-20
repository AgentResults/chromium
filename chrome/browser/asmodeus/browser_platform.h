// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_BROWSER_PLATFORM_H_
#define CHROME_BROWSER_ASMODEUS_BROWSER_PLATFORM_H_

#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/asmodeus/meeting_backend.h"

namespace content {
class BrowserContext;
}

namespace asmodeus {

class AsmodeusParticipant;

// MeetingBackend for browser-based platforms (Meet, Teams, Slack, etc.).
// Creates an AsmodeusParticipant per agent, navigates to the meeting URL,
// and injects platform-specific join/features scripts.
//
// The actual audio path is shared across all browser platforms:
//   Chrome WebRTC → ForwardingAudioStreamFactory → AsmodeusAudioOutput → shm
//
// Each platform only differs in its join automation (DOM selectors, click flow).
class BrowserPlatform : public MeetingBackend {
 public:
  // |platform_id|: "meet", "teams", "slack", "zoom", "discord", "whatsapp"
  // |browser_context|: the Chrome browser context for creating WebContents
  BrowserPlatform(const std::string& platform_id,
                  content::BrowserContext* browser_context);
  ~BrowserPlatform() override;

  bool Start(const std::string& meeting_url) override;
  void Stop() override;
  bool ConnectAgent(const std::string& name,
                    const std::string& audio_shm_path,
                    const std::string& video_shm_path) override;
  void DisconnectAgent(const std::string& name) override;
  bool is_running() const override;

  const std::string& platform_id() const { return platform_id_; }
  const std::string& meeting_url() const { return meeting_url_; }

  // Auto-detect platform from URL.
  static std::string DetectPlatform(const std::string& url);

 private:
  std::string platform_id_;
  std::string meeting_url_;
  raw_ptr<content::BrowserContext> browser_context_;
  std::map<std::string, std::unique_ptr<AsmodeusParticipant>> participants_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_BROWSER_PLATFORM_H_
