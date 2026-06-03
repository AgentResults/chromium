// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/asmodeus_participant.h"

#include <sys/stat.h>

#include "base/logging.h"
#include "base/process/process.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/mojom/frame/user_activation_notification_type.mojom.h"
#include "content/public/browser/render_process_host.h"
#include "media/audio/asmodeus/asmodeus_participant_registry.h"
#include "media/audio/asmodeus/asmodeus_virtual_devices.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom.h"
#include "ui/gfx/geometry/rect.h"

namespace asmodeus {

AsmodeusParticipant::AsmodeusParticipant(
    const std::string& name,
    content::BrowserContext* browser_context,
    const std::string& audio_device,
    const std::string& video_device,
    bool use_incognito,
    const std::string& profile_path,
    const std::string& account_email)
    : name_(name),
      audio_device_(audio_device.empty() ? "default" : audio_device),
      video_device_(video_device.empty() ? "default" : video_device) {
  // Create participant with isolated OTR profile.
  // Each participant gets fully isolated cookies/network/WebRTC.
  Profile* profile = Profile::FromBrowserContext(browser_context);
  content::BrowserContext* ctx = browser_context;
  if (profile) {
    ctx = profile->GetOffTheRecordProfile(
        Profile::OTRProfileID::CreateUniqueForTesting(),
        /*create_if_needed=*/true);
  }
  content::WebContents::CreateParams params(ctx);
  web_contents_ = content::WebContents::Create(params);
  web_contents_->SetDelegate(this);
  Observe(web_contents_.get());

  // Expose as DevTools target so we can inspect/screenshot via CDP.
  content::DevToolsAgentHost::GetOrCreateFor(web_contents_.get());

  // Force the WebContents to be treated as visible and active.
  // This prevents Chrome from throttling WebRTC audio/video.
  web_contents_->SetAudioMuted(false);
  web_contents_->WasShown();
  // Resize to non-zero to prevent renderer throttling
  web_contents_->Resize(gfx::Rect(0, 0, 1280, 720));

  // Register per-participant virtual output device (audio-out-{name}.shm).
  // This makes MakeLowLatencyOutputStream route this participant's audio
  // to its own shm file instead of the real speakers.
  std::string media_dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.asmodeus";
  std::string out_shm = media_dir + "/audio-out-" + name + ".shm";
  asmodeus::RegisterVirtualOutputDevice(name, out_shm);

  LOG(WARNING) << "[Asmodeus] Participant '" << name << "' created"
               << " audio=" << audio_device_ << " video=" << video_device_
               << " output_shm=" << out_shm
               << " incognito=" << use_incognito;
}

AsmodeusParticipant::~AsmodeusParticipant() {
  asmodeus::UnregisterVirtualOutputDevice(name_);
  asmodeus::ParticipantRegistry::Get().UnregisterByName(name_);
  LOG(WARNING) << "[Asmodeus] Participant '" << name_ << "' destroyed";
}

void AsmodeusParticipant::Navigate(const GURL& url) {
  loaded_ = false;

  content::NavigationController::LoadURLParams load_params(url);
  web_contents_->GetController().LoadURLWithParams(load_params);
  LOG(WARNING) << "[Asmodeus] Participant '" << name_
               << "' navigating to: " << url.spec();
}

void AsmodeusParticipant::ExecuteJS(
    const std::u16string& script,
    content::RenderFrameHost::JavaScriptResultCallback callback) {
  auto* frame = web_contents_->GetPrimaryMainFrame();
  if (!frame || !frame->IsRenderFrameLive()) {
    LOG(ERROR) << "[Asmodeus] Participant '" << name_
               << "': frame not ready for JS execution";
    if (callback) {
      std::move(callback).Run(base::Value());
    }
    return;
  }
  frame->ExecuteJavaScriptForTests(script, std::move(callback),
                                   /*world_id=*/0);
}

void AsmodeusParticipant::RequestMediaAccessPermission(
    content::WebContents* web_contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback callback) {
  blink::mojom::StreamDevicesSet stream_devices_set;
  stream_devices_set.stream_devices.emplace_back(
      blink::mojom::StreamDevices::New());
  auto& devices = *stream_devices_set.stream_devices[0];

  if (request.audio_type != blink::mojom::MediaStreamType::NO_SERVICE) {
    // Use "default" as device ID so the renderer's eligible_audio_settings
    // matches. The actual audio routing to SHM happens at the output stage
    // via AsmodeusAudioOutput and the virtual output device.
    devices.audio_device = blink::MediaStreamDevice(
        request.audio_type, "default", "Default");
    LOG(WARNING) << "[Asmodeus] Participant '" << name_
                 << "' audio: default (routed via virtual output)";
  }
  if (request.video_type != blink::mojom::MediaStreamType::NO_SERVICE) {
    devices.video_device = blink::MediaStreamDevice(
        request.video_type, "default", "Default");
    LOG(WARNING) << "[Asmodeus] Participant '" << name_
                 << "' video: default";
  }

  std::move(callback).Run(
      stream_devices_set,
      blink::mojom::MediaStreamRequestResult::OK,
      /*ui=*/nullptr);
}

bool AsmodeusParticipant::CheckMediaAccessPermission(
    content::RenderFrameHost* render_frame_host,
    const url::Origin& security_origin,
    blink::mojom::MediaStreamType type) {
  return true;
}

void AsmodeusParticipant::OnVisibilityChanged(content::Visibility visibility) {
  // NEVER let the WebContents become hidden — this prevents Chrome from
  // muting WebRTC audio tracks. Force back to VISIBLE immediately.
  if (visibility != content::Visibility::VISIBLE && web_contents_) {
    web_contents_->UpdateWebContentsVisibility(content::Visibility::VISIBLE);
  }
}

void AsmodeusParticipant::ReadyToCommitNavigation(
    content::NavigationHandle* navigation_handle) {
  // Register frames EARLY — before DidFinishNavigation — so that
  // ForwardingAudioStreamFactory can route audio for any streams
  // created during navigation (e.g., getUserMedia called by Meet
  // before the page fully loads). This prevents the race condition
  // where audio streams are created before the frame is registered
  // in ParticipantRegistry.
  auto* nav_frame = navigation_handle->GetRenderFrameHost();
  if (nav_frame) {
    asmodeus::ParticipantRegistry::Get().Register(
        nav_frame->GetProcess()->GetDeprecatedID(),
        nav_frame->GetRoutingID(),
        name_);
    LOG(WARNING) << "[Asmodeus] Early registration for '" << name_
                 << "' frame (" << nav_frame->GetProcess()->GetDeprecatedID()
                 << "," << nav_frame->GetRoutingID() << ") at ReadyToCommit";

    // Override getUserMedia BEFORE Meet's JS runs.
    // Must be in ReadyToCommitNavigation, not DidFinishNavigation,
    // because Meet calls getUserMedia during page load.
    // Disable WebRTC APM (echo cancellation, noise suppression, AGC)
    // which strips the clean TTS audio from the virtual mic.
    if (navigation_handle->IsInPrimaryMainFrame() &&
        nav_frame->IsRenderFrameLive()) {
      nav_frame->ExecuteJavaScriptWithUserGestureForTests(
          u"(function(){"
          u"if(window.__asmodeusGUMHooked)return;"
          u"window.__asmodeusGUMHooked=true;"
          u"var origGUM=navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);"
          u"navigator.mediaDevices.getUserMedia=function(c){"
          u"if(c&&c.audio){"
          u"if(typeof c.audio==='object'){"
          u"c.audio.autoGainControl=false;"
          u"c.audio.echoCancellation=false;"
          u"c.audio.noiseSuppression=false;"
          u"}else{"
          u"c.audio={autoGainControl:false,echoCancellation:false,noiseSuppression:false};"
          u"}"
          u"}"
          u"return origGUM(c);"
          u"};"
          u"})();",
          base::NullCallback(), /*world_id=*/0);
    }
  }
}

void AsmodeusParticipant::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (navigation_handle->HasCommitted()) {
    auto* nav_frame = navigation_handle->GetRenderFrameHost();
    if (nav_frame) {
      // Register EVERY frame (main + iframes) for per-participant audio routing.
      // Meet uses iframes for WebRTC, so we must route all frames' audio output
      // to the participant's virtual output device.
      asmodeus::ParticipantRegistry::Get().Register(
          nav_frame->GetProcess()->GetDeprecatedID(),
          nav_frame->GetRoutingID(),
          name_);
      LOG(WARNING) << "[Asmodeus] Participant '" << name_
                   << "' nav: " << navigation_handle->GetURL().spec()
                   << " isMain=" << navigation_handle->IsInPrimaryMainFrame();

      // Simulate user activation for the main frame.
      // reCAPTCHA checks for user interaction signals. Without activation,
      // the invisible challenge fails silently and the join button doesn't work.
      if (navigation_handle->IsInPrimaryMainFrame() &&
          nav_frame->IsRenderFrameLive()) {
        // Notify the frame that a user gesture occurred.
        nav_frame->NotifyUserActivation(
            blink::mojom::UserActivationNotificationType::kInteraction);

        // Send a simulated mouse move to create interaction signals
        auto* wc = web_contents_.get();
        if (wc && wc->GetRenderWidgetHostView()) {
          auto* rwh = wc->GetRenderWidgetHostView()->GetRenderWidgetHost();
          if (rwh) {
            blink::WebMouseEvent move(
                blink::WebInputEvent::Type::kMouseMove,
                blink::WebInputEvent::kNoModifiers,
                base::TimeTicks::Now());
            move.SetPositionInWidget(gfx::PointF(640, 360));
            rwh->ForwardMouseEvent(move);
          }
        }
        LOG(WARNING) << "[Asmodeus] User activation simulated for " << name_;
      }

      // Write PID mapping so the renderer's WebRtcAudioRenderer can find
      // its shm file. Maps OS PID → participant name.
      base::ProcessId os_pid = nav_frame->GetProcess()->GetProcess().Pid();
      if (os_pid > 0) {
        std::string pids_dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp")
                             + "/.asmodeus/pids";
        mkdir(pids_dir.c_str(), 0755);
        std::string pid_path = pids_dir + "/" + std::to_string(os_pid);
        FILE* f = fopen(pid_path.c_str(), "w");
        if (f) {
          fprintf(f, "%s\n", name_.c_str());
          fclose(f);
          LOG(WARNING) << "[Asmodeus] PID mapping: " << os_pid
                       << " → " << name_;
        }
      }
    }
    // Inject RTC hook into EVERY frame (main + iframes).
    // MUST use ExecuteJavaScriptWithUserGestureForTests so that
    // AudioContext creation is allowed (autoplay policy requires
    // a user gesture). Without this, AudioContext stays suspended
    // and remote WebRTC audio never reaches the virtual output device.
    if (nav_frame && nav_frame->IsRenderFrameLive()) {
      nav_frame->ExecuteJavaScriptWithUserGestureForTests(
          u"(function(){"
          u"if(window.__asmodeusRTCHooked)return;"
          u"window.__asmodeusRTCHooked=true;"
          u"window.__asmodeusPCs=window.__asmodeusPCs||[];"
          u"window.__asmodeusSources=window.__asmodeusSources||[];"
          u"var OrigRTC=window.RTCPeerConnection;"
          u"if(!OrigRTC)return;"
          u"function attachTrack(track){"
          u"if(track.kind!=='audio')return;"
          u"if(!window.__asmodeusCtx){"
          u"window.__asmodeusCtx=new AudioContext({sampleRate:48000});"
          u"if(window.__asmodeusCtx.state==='suspended')window.__asmodeusCtx.resume();"
          u"}"
          u"var ac=window.__asmodeusCtx;"
          u"if(ac.state==='suspended')ac.resume();"
          u"try{"
          u"var src=ac.createMediaStreamSource(new MediaStream([track]));"
          u"src.connect(ac.destination);"
          u"window.__asmodeusSources.push(src);"
          u"}catch(e){}"
          u"}"
          u"function WrappedRTC(config){"
          u"var pc=new OrigRTC(config);"
          u"window.__asmodeusPCs.push(pc);"
          u"pc.addEventListener('track',function(e){"
          u"if(e.track&&!e.track.__asmodeusAttached){"
          u"e.track.__asmodeusAttached=true;"
          u"attachTrack(e.track);"
          u"}"
          u"});"
          u"return pc;"
          u"}"
          u"WrappedRTC.prototype=OrigRTC.prototype;"
          u"if(OrigRTC.generateCertificate)WrappedRTC.generateCertificate=OrigRTC.generateCertificate;"
          u"window.RTCPeerConnection=WrappedRTC;"
          u"setInterval(function(){"
          u"for(var i=0;i<window.__asmodeusPCs.length;i++){"
          u"try{"
          u"var recv=window.__asmodeusPCs[i].getReceivers();"
          u"for(var j=0;j<recv.length;j++){"
          u"if(recv[j].track&&recv[j].track.kind==='audio'&&!recv[j].track.__asmodeusAttached){"
          u"recv[j].track.__asmodeusAttached=true;"
          u"attachTrack(recv[j].track);"
          u"}"
          u"}"
          u"}catch(e){}"
          u"}"
          u"},2000);"
          u"})();",
          base::NullCallback(), 0);
    }

    if (navigation_handle->IsInPrimaryMainFrame()) {
    loaded_ = true;
    auto* frame = web_contents_->GetPrimaryMainFrame();
    if (frame) {

      // Inject getUserMedia hook:
      // 1. Disable audio processing (AEC/NS/AGC)
      // 2. Set per-participant camera by matching device label
      // The participant name is embedded in the hook at injection time.
      {
        std::u16string participant_name16 =
            base::UTF8ToUTF16(name_);
        frame->ExecuteJavaScriptWithUserGestureForTests(
            u"(function(){if(window.__gumHooked)return;window.__gumHooked=true;"
            u"var participantName='" + participant_name16 + u"';"
            u"var o=navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);"
            u"navigator.mediaDevices.getUserMedia=async function(c){"
            u"if(c&&c.audio){"
            u"if(typeof c.audio==='boolean'){c.audio={echoCancellation:false,noiseSuppression:false,autoGainControl:false};}"
            u"else if(typeof c.audio==='object'){c.audio.echoCancellation=false;c.audio.noiseSuppression=false;c.audio.autoGainControl=false;}"
            u"}"
            // Find this participant's camera by label and set deviceId
            u"if(c&&c.video){"
            u"try{"
            u"var devs=await navigator.mediaDevices.enumerateDevices();"
            u"var cam=devs.find(function(d){return d.kind==='videoinput'&&d.label.toLowerCase().indexOf(participantName.toLowerCase())>=0;});"
            u"if(cam){"
            u"if(typeof c.video==='boolean'){c.video={deviceId:{exact:cam.deviceId}};}"
            u"else if(typeof c.video==='object'){c.video.deviceId={exact:cam.deviceId};}"
            u"}"
            u"}catch(e){}"
            u"}"
            u"return o(c);"
            u"};})();",
            base::NullCallback(), 0);
      }

      // 2. Auto-dismiss all popups, dialogs, and notifications.
      // Runs every 2s to catch dynamically appearing popups.
      frame->ExecuteJavaScriptForTests(
          u"(function(){if(window.__popupDismisser)return;window.__popupDismisser=true;"
          u"setInterval(function(){"
          u"var dismiss=['Not now','Dismiss','Close','Got it','No thanks','Maybe later','Skip','OK'];"
          u"document.querySelectorAll('button,[role=button]').forEach(function(b){"
          u"var t=b.textContent.trim();"
          u"for(var d of dismiss){if(t===d||t.toLowerCase()===d.toLowerCase()){b.click();return;}}"
          u"});"
          u"document.querySelectorAll('[aria-label=Close],[aria-label=Dismiss]').forEach(function(b){b.click();});"
          u"document.querySelectorAll('[role=dialog] button,[role=alertdialog] button').forEach(function(b){"
          u"var t=b.textContent.trim().toLowerCase();"
          u"if(t==='not now'||t==='dismiss'||t==='close'||t==='got it')b.click();"
          u"});"
          u"},2000);})();",
          base::NullCallback(), 0);

      // 3. Override visibility APIs so Meet doesn't throttle hidden tabs
      frame->ExecuteJavaScriptForTests(
          u"Object.defineProperty(document,'hasFocus',{value:function(){return true}});"
          u"Object.defineProperty(document,'hidden',{get:function(){return false}});"
          u"Object.defineProperty(document,'visibilityState',{get:function(){return'visible'}});"
          u"Object.defineProperty(navigator,'webdriver',{get:function(){return undefined}});",
          base::NullCallback(), 0);

      // 4. Force WebRTC audio playout through AudioContext.
      // Meet may bypass our RTCPeerConnection wrapper. Instead, scan for
      // ALL audio/video elements and <audio> sources, and route them
      // through AudioContext.destination to force through our virtual
      // output device.
      // MUST use WithUserGestureForTests — AudioContext needs user gesture.
      frame->ExecuteJavaScriptWithUserGestureForTests(
          u"(function(){"
          u"if(window.__asmodeusAudioScanner)return;"
          u"window.__asmodeusAudioScanner=true;"
          u"setInterval(function(){"
          // Scan for HTMLAudioElement and HTMLVideoElement playing audio
          u"document.querySelectorAll('audio,video').forEach(function(el){"
          u"if(!el.__asmodeusAttached&&el.srcObject){"
          u"el.__asmodeusAttached=true;"
          u"if(!window.__asmodeusCtx){"
          u"window.__asmodeusCtx=new AudioContext({sampleRate:48000});"
          u"window.__asmodeusCtx.resume();"
          u"}"
          u"var ac=window.__asmodeusCtx;"
          u"if(ac.state==='suspended')ac.resume();"
          u"try{"
          u"var src=ac.createMediaElementSource(el);"
          u"src.connect(ac.destination);"
          u"}catch(e){}"
          u"}"
          u"});"
          u"},1000);"
          u"})();",
          base::NullCallback(), 0);
    }
    LOG(WARNING) << "[Asmodeus] Participant '" << name_
                 << "' loaded + hooks injected: " << navigation_handle->GetURL().spec();
    }  // if (IsInPrimaryMainFrame())
  }
}

}  // namespace asmodeus
