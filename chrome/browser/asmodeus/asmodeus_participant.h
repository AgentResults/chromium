// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_ASMODEUS_PARTICIPANT_H_
#define CHROME_BROWSER_ASMODEUS_ASMODEUS_PARTICIPANT_H_

#include <memory>
#include <string>

#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"

namespace asmodeus {

// A headless meeting participant — creates a WebContents without a visible
// window, loads a Meet URL, and routes virtual devices to/from it.
//
// External processes write audio/video to shm files; the participant's
// WebContents uses them via the virtual mic/camera. Audio output from
// the WebContents is captured to a separate shm for STT.
class AsmodeusParticipant : public content::WebContentsDelegate,
                            public content::WebContentsObserver {
 public:
  // profile_path: if non-empty, use a persistent Chrome profile at this path
  //               instead of creating an OTR (incognito) profile.
  //               This allows each agent to have its own Google session.
  // account_email: Google account email for auto sign-in detection.
  AsmodeusParticipant(const std::string& name,
                      content::BrowserContext* browser_context,
                      const std::string& audio_device = "",
                      const std::string& video_device = "",
                      bool use_incognito = false,
                      const std::string& profile_path = "",
                      const std::string& account_email = "");
  ~AsmodeusParticipant() override;

  AsmodeusParticipant(const AsmodeusParticipant&) = delete;
  AsmodeusParticipant& operator=(const AsmodeusParticipant&) = delete;

  // Navigate to a URL (e.g., a Meet link).
  void Navigate(const GURL& url);

  // Execute JavaScript in the page's global world.
  // Callback receives the result as a base::Value.
  void ExecuteJS(const std::u16string& script,
                 content::RenderFrameHost::JavaScriptResultCallback callback);

  // Get the underlying WebContents (for DevTools agent attachment).
  content::WebContents* web_contents() { return web_contents_.get(); }

  const std::string& name() const { return name_; }
  bool is_loaded() const { return loaded_; }

  // WebContentsDelegate overrides — auto-grant media permissions.
  void RequestMediaAccessPermission(
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      content::MediaResponseCallback callback) override;
  bool CheckMediaAccessPermission(
      content::RenderFrameHost* render_frame_host,
      const url::Origin& security_origin,
      blink::mojom::MediaStreamType type) override;

  // WebContentsObserver overrides.
  void ReadyToCommitNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;
  void OnVisibilityChanged(content::Visibility visibility) override;

 private:
  std::string name_;
  std::string audio_device_;
  std::string video_device_;
  std::unique_ptr<content::WebContents> web_contents_;
  bool loaded_ = false;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_ASMODEUS_PARTICIPANT_H_
