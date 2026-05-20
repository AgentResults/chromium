// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DEVTOOLS_PROTOCOL_ASMODEUS_HANDLER_H_
#define CHROME_BROWSER_DEVTOOLS_PROTOCOL_ASMODEUS_HANDLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chrome/browser/asmodeus/asmodeus_audio_capture.h"
#include "chrome/browser/asmodeus/asmodeus_media_server.h"
#include "chrome/browser/asmodeus/asmodeus_meeting_server.h"
#include "chrome/browser/asmodeus/auth_controller.h"
#include "chrome/browser/asmodeus/credential_store.h"
#include "chrome/browser/asmodeus/instance_manager.h"
#include "chrome/browser/asmodeus/meeting_coordinator.h"
#include "chrome/browser/asmodeus/page_controller.h"
#include "chrome/browser/asmodeus/permission_override.h"
#include "chrome/browser/asmodeus/asmodeus_participant.h"
#include "chrome/browser/devtools/protocol/asmodeus.h"
#include "components/password_manager/core/browser/password_store/password_store_consumer.h"
#include "content/public/browser/peer_connection_tracker_host_observer.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"

namespace password_manager {
class PasswordStoreInterface;
}

class AsmodeusHandler : public protocol::Asmodeus::Backend,
                        public content::WebContentsObserver,
                        public content::PeerConnectionTrackerHostObserver {
 public:
  using DispatchResponse = protocol::DispatchResponse;
  using String = protocol::String;

  AsmodeusHandler(protocol::UberDispatcher* dispatcher,
                  content::WebContents* web_contents);
  ~AsmodeusHandler() override;

  AsmodeusHandler(const AsmodeusHandler&) = delete;
  AsmodeusHandler& operator=(const AsmodeusHandler&) = delete;

  DispatchResponse Enable() override;
  DispatchResponse Disable() override;
  DispatchResponse SuppressFingerprints(bool in_suppress) override;

  void GetCredentials(const String& in_origin,
                      std::unique_ptr<GetCredentialsCallback> callback) override;
  void SaveCredentials(const String& in_origin, const String& in_username,
                       const String& in_password,
                       std::optional<String> in_totpSecret,
                       std::optional<String> in_notes,
                       std::unique_ptr<SaveCredentialsCallback> callback) override;
  void DeleteCredentials(const String& in_origin, const String& in_username,
                         std::unique_ptr<DeleteCredentialsCallback> callback) override;
  void ListCredentials(std::unique_ptr<ListCredentialsCallback> callback) override;

  DispatchResponse GenerateTOTP(const String& in_secret,
                                std::optional<int> in_digits,
                                std::optional<int> in_period,
                                String* out_code,
                                double* out_remainingSeconds) override;

  DispatchResponse DetectAuthFlow(
      std::unique_ptr<protocol::Asmodeus::AuthFlow>* out_flow) override;
  DispatchResponse GetAuthState(
      std::unique_ptr<protocol::Array<protocol::Asmodeus::AuthFlow>>*
          out_activeFlows) override;
  DispatchResponse DetectLoginForm(
      std::unique_ptr<protocol::Asmodeus::LoginForm>* out_form) override;

  void AutoLogin(const String& in_origin, std::optional<String> in_username,
                 std::unique_ptr<AutoLoginCallback> callback) override;
  void ExportSession(std::optional<String> in_origin,
                     std::unique_ptr<ExportSessionCallback> callback) override;
  void ImportSession(
      std::unique_ptr<protocol::Array<protocol::Asmodeus::SessionSnapshot>> in_sessions,
      std::unique_ptr<ImportSessionCallback> callback) override;

  // Virtual audio — per-device named virtual microphones
  DispatchResponse EnableVirtualAudio(std::optional<String> in_name,
                                      std::optional<int> in_sampleRate,
                                      std::optional<int> in_channels,
                                      String* out_deviceId,
                                      String* out_shmPathIn,
                                      String* out_shmPathOut) override;
  DispatchResponse DisableVirtualAudio(
      std::optional<String> in_name) override;
  DispatchResponse GetVirtualAudioStatus(std::optional<String> in_name,
                                         bool* out_enabled,
                                         std::optional<String>* out_shmPathIn,
                                         std::optional<String>* out_shmPathOut,
                                         std::optional<int>* out_sampleRate,
                                         std::optional<int>* out_channels) override;

  // Headless participants
  DispatchResponse CreateParticipant(const String& in_name,
                                     const String& in_url,
                                     std::optional<String> in_displayName,
                                     String* out_participantId,
                                     bool* out_loaded) override;
  void EvaluateInParticipant(const String& in_name,
                             const String& in_expression,
                             std::unique_ptr<EvaluateInParticipantCallback> callback) override;
  DispatchResponse GetParticipantState(const String& in_name,
                                       bool* out_exists,
                                       bool* out_loaded,
                                       std::optional<String>* out_url) override;
  DispatchResponse DestroyParticipant(const String& in_name) override;

  // Virtual camera — per-device named virtual cameras
  DispatchResponse EnableVirtualCamera(std::optional<String> in_name,
                                       std::optional<int> in_width,
                                       std::optional<int> in_height,
                                       std::optional<int> in_fps,
                                       String* out_deviceId,
                                       String* out_shmPath) override;
  DispatchResponse DisableVirtualCamera(
      std::optional<String> in_name) override;

  void DidStartNavigation(content::NavigationHandle* navigation_handle) override;
  void DidFinishNavigation(content::NavigationHandle* navigation_handle) override;

  // ── Page interaction ───────────────────────────────────────
  DispatchResponse TypeText(const String& in_text) override;
  DispatchResponse ClickByAriaLabel(const String& in_labelSubstring,
                                     bool* out_found,
                                     std::optional<String>* out_clickedLabel) override;

  // ── Traffic capture ────────────────────────────────────────
  DispatchResponse CaptureStart(
      std::unique_ptr<protocol::Array<String>> in_includeUrlPatterns,
      std::unique_ptr<protocol::Array<String>> in_excludeUrlPatterns,
      std::optional<bool> in_captureWebRtc,
      std::optional<bool> in_captureWebSocketFrames,
      std::optional<bool> in_captureHttp) override;
  DispatchResponse CaptureStop() override;

  // Tab audio capture
  DispatchResponse CaptureTabAudio(const String& in_outputPath,
                                    std::optional<int> in_sampleRate,
                                    std::optional<int> in_channels) override;
  DispatchResponse StopAudioCapture(double* out_durationMs,
                                     int* out_samples,
                                     double* out_peakRms,
                                     String* out_outputPath) override;
  DispatchResponse GetAudioLevel(double* out_rms,
                                  double* out_peak,
                                  bool* out_capturing) override;

  // Meeting management
  DispatchResponse CreateMeeting(std::optional<String> in_name,
                                  std::optional<String> in_meetingHtml,
                                  String* out_meetingUrl,
                                  String* out_signalingUrl,
                                  int* out_port) override;
  DispatchResponse AddAgent(const String& in_name,
                             std::optional<String> in_voiceModel,
                             std::optional<String> in_displayName,
                             String* out_participantId,
                             String* out_audioShmPath,
                             String* out_videoShmPath,
                             bool* out_connected,
                             int* out_controlPort) override;
  DispatchResponse RemoveAgent(const String& in_name) override;
  DispatchResponse GetMeetingState(
      std::unique_ptr<protocol::Array<protocol::Asmodeus::AgentState>>* out_agents,
      int* out_signalingPeers,
      bool* out_recording) override;
  DispatchResponse JoinMeeting(const String& in_url,
                               std::optional<String> in_platform,
                               bool* out_joined,
                               String* out_platform) override;
  DispatchResponse GetTranscript(
      std::unique_ptr<protocol::Array<protocol::Asmodeus::TranscriptEntry>>*
          out_entries) override;
  DispatchResponse Speak(const String& in_name,
                          const String& in_text,
                          bool* out_queued) override;
  DispatchResponse StartRecording(const String& in_outputDir,
                                   bool* out_started) override;
  DispatchResponse StopRecording(int* out_frames,
                                  int* out_audioSamples,
                                  double* out_peakRms,
                                  String* out_mp4Path) override;

  // ── General-purpose page interaction ────────────────────────
  DispatchResponse ClickElement(const String& in_target,
                                 std::optional<String> in_method,
                                 bool* out_found,
                                 std::optional<String>* out_elementTag,
                                 std::optional<String>* out_elementText) override;
  DispatchResponse TypeInto(const String& in_text,
                             std::optional<String> in_selector,
                             bool* out_success) override;
  DispatchResponse ReadElement(const String& in_selector,
                                std::optional<String>* out_text,
                                std::optional<String>* out_value,
                                std::optional<String>* out_tag) override;
  DispatchResponse GetPageInfo(String* out_url,
                                String* out_title) override;

  // ── Authentication automation ───────────────────────────────
  DispatchResponse SignIn(const String& in_agentName,
                           bool* out_started,
                           std::optional<String>* out_currentPage) override;
  DispatchResponse Enter2FACode(const String& in_code,
                                 bool* out_submitted) override;
  DispatchResponse GetSignInState(bool* out_signingIn,
                                   std::optional<String>* out_currentPage,
                                   std::optional<String>* out_agentName) override;

  // ── Permission management ───────────────────────────────────
  DispatchResponse SetAutoGrantPermissions(bool in_enabled,
                                            bool* out_success) override;
  DispatchResponse SetPermission(const String& in_permission,
                                  bool in_granted,
                                  bool* out_success) override;

  // ── Instance management ─────────────────────────────────────
  DispatchResponse LaunchInstance(const String& in_agentName,
                                   std::optional<String> in_profilePath,
                                   std::optional<int> in_port,
                                   int* out_cdpPort,
                                   String* out_profilePath,
                                   String* out_audioInShm,
                                   String* out_audioOutShm) override;
  DispatchResponse StopInstance(const String& in_agentName) override;
  DispatchResponse ListInstances(
      std::unique_ptr<protocol::Array<protocol::Asmodeus::AgentState>>*
          out_instances) override;

  // content::PeerConnectionTrackerHostObserver
  void OnPeerConnectionAdded(content::GlobalRenderFrameHostId render_frame_host_id,
                             int lid, base::ProcessId pid,
                             const std::string& url,
                             const std::string& rtc_configuration) override;
  void OnPeerConnectionRemoved(content::GlobalRenderFrameHostId render_frame_host_id,
                               int lid) override;
  void OnPeerConnectionUpdated(content::GlobalRenderFrameHostId render_frame_host_id,
                               int lid, const std::string& type,
                               const std::string& value) override;

 private:
  class CredentialConsumer;

  password_manager::PasswordStoreInterface* GetPasswordStore();
  std::string ComputeTOTP(const std::string& base32_secret, int digits,
                          int period, int64_t* remaining_seconds);
  std::vector<uint8_t> Base32Decode(const std::string& input);

  struct AuthProviderMatch { std::string provider; std::string type; };
  std::optional<AuthProviderMatch> MatchAuthProvider(const GURL& url);
  void InjectStealthScript();

  bool enabled_ = false;
  bool fingerprints_suppressed_ = false;

  struct ActiveFlow {
    ActiveFlow();
    ActiveFlow(const ActiveFlow&);
    ActiveFlow(ActiveFlow&&);
    ActiveFlow& operator=(const ActiveFlow&);
    ActiveFlow& operator=(ActiveFlow&&);
    ~ActiveFlow();
    std::string type;
    std::string origin;
    std::string state;
    std::string provider;
  };
  std::vector<ActiveFlow> active_flows_;
  std::string pre_auth_origin_;

  std::vector<std::unique_ptr<CredentialConsumer>> pending_consumers_;
  // Named virtual audio devices — one AsmodeusMediaServer per device name.
  std::map<std::string, std::unique_ptr<asmodeus::AsmodeusMediaServer>>
      media_servers_;
  // Named virtual camera shm paths (managed separately from audio).
  std::map<std::string, std::string> video_shm_paths_;
  // Headless meeting participants.
  std::map<std::string, std::unique_ptr<asmodeus::AsmodeusParticipant>>
      participants_;
  std::unique_ptr<protocol::Asmodeus::Frontend> frontend_;
  raw_ptr<content::WebContents> web_contents_;

  // Traffic capture state.
  bool capture_active_ = false;
  bool capture_webrtc_ = true;
  bool capture_ws_ = true;
  bool capture_http_ = true;
  std::vector<std::string> include_patterns_;
  std::vector<std::string> exclude_patterns_;
  // url_by_pc_[pid][lid] = frame url seen at OnPeerConnectionAdded time.
  std::map<std::pair<base::ProcessId, int>, std::string> url_by_pc_;

  // Meeting server (embedded signaling + HTTP) — legacy, kept for backward compat.
  std::unique_ptr<asmodeus::AsmodeusMeetingServer> meeting_server_;
  std::string meeting_html_path_;

  // Meeting coordinator (new system — spawns native agents, composites, records).
  std::unique_ptr<asmodeus::MeetingCoordinator> coordinator_;

  // Tab audio capture
  std::unique_ptr<asmodeus::AsmodeusAudioCapture> audio_capture_;

  // General-purpose automation components
  asmodeus::CredentialStore credential_store_;
  std::unique_ptr<asmodeus::AuthController> auth_controller_;
  asmodeus::PageController page_controller_;
  asmodeus::PermissionOverride permission_override_;
  std::unique_ptr<asmodeus::InstanceManager> instance_manager_;
};

#endif  // CHROME_BROWSER_DEVTOOLS_PROTOCOL_ASMODEUS_HANDLER_H_
