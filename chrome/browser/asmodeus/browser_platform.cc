// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/browser_platform.h"

#include <sys/stat.h>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/json/json_reader.h"
#include "chrome/browser/asmodeus/asmodeus_media_server.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "ui/gfx/geometry/point_f.h"
#include "chrome/browser/asmodeus/asmodeus_participant.h"
#include "media/audio/asmodeus/audio_ring_buffer.h"
#include "url/gurl.h"

namespace asmodeus {

BrowserPlatform::BrowserPlatform(const std::string& platform_id,
                                 content::BrowserContext* browser_context)
    : platform_id_(platform_id), browser_context_(browser_context) {}

BrowserPlatform::~BrowserPlatform() {
  Stop();
}

bool BrowserPlatform::is_running() const {
  return !meeting_url_.empty();
}

bool BrowserPlatform::Start(const std::string& meeting_url) {
  meeting_url_ = meeting_url;
  LOG(INFO) << "BrowserPlatform[" << platform_id_ << "] started for "
            << meeting_url;
  return true;
}

void BrowserPlatform::Stop() {
  for (auto& [name, participant] : participants_) {
    LOG(INFO) << "BrowserPlatform[" << platform_id_
              << "] disconnecting " << name;
  }
  participants_.clear();
  LOG(INFO) << "BrowserPlatform[" << platform_id_ << "] stopped";
}

bool BrowserPlatform::ConnectAgent(const std::string& name,
                                    const std::string& audio_shm_path,
                                    const std::string& video_shm_path) {
  if (meeting_url_.empty()) {
    LOG(ERROR) << "BrowserPlatform: no meeting URL set";
    return false;
  }

  // Ensure shm files exist before participant or agent starts.
  // audio_shm_path = audio-in-{name}.shm (agent TTS -> Chrome virtual mic)
  // audio-out-{name}.shm = Chrome decoded WebRTC audio -> agent reads
  std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
  std::string media_dir = home + "/.asmodeus";
  mkdir(media_dir.c_str(), 0755);  // raw mkdir — no DCHECK on UI thread

  // Derive audio-out path from audio-in path
  std::string audio_out_path = media_dir + "/audio-out-" + name + ".shm";

  // Pre-create both SHM files so agent can Open them without truncation.
  // CreateIfNeeded: creates if missing, opens if already exists.
  {
    AudioRingBuffer tmp;
    tmp.CreateIfNeeded(audio_shm_path, 48000, 1, 48000 * 10);
    tmp.Close();
    tmp.CreateIfNeeded(audio_out_path, 48000, 1, 48000 * 10);
    tmp.Close();
  }
  LOG(ERROR) << "BrowserPlatform: ensured shm files for " << name;

  // Create participant with virtual audio/video devices
  std::string audio_device = "asmodeus-" + name;
  std::string video_device = "asmodeus-cam-" + name;

  auto participant = std::make_unique<AsmodeusParticipant>(
      name, browser_context_, audio_device, video_device,
      /*use_incognito=*/true);

  // Navigate to the meeting URL
  participant->Navigate(GURL(meeting_url_));

  // Schedule join script injection after page loads.
  // The participant's DidFinishNavigation sets loaded_=true.
  auto* raw_participant = participant.get();
  std::string display_name = name;
  if (!display_name.empty())
    display_name[0] = toupper(display_name[0]);
  std::string pid = platform_id_;

  participants_[name] = std::move(participant);

  // Post a delayed task to inject the join script.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](AsmodeusParticipant* p, std::string platform_id,
             std::string agent_name, std::string display) {
            if (!p || !p->is_loaded()) {
              LOG(ERROR) << "BrowserPlatform: page not loaded yet for "
                           << agent_name;
              return;
            }

            // Load join.js from platforms directory (raw I/O to avoid DCHECK)
            std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
            std::string join_path = home +
                "/workspace/Legion/Asmodeus/platforms/" + platform_id +
                "/join.js";
            std::string join_js;
            FILE* f = fopen(join_path.c_str(), "r");
            if (!f) {
              LOG(ERROR) << "BrowserPlatform: FAILED to read join script: " << join_path;
              return;
            }
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
              join_js.append(buf, n);
            fclose(f);

            // Inject the script
            p->ExecuteJS(base::UTF8ToUTF16(join_js), base::NullCallback());
            LOG(ERROR) << "BrowserPlatform: INJECTED join script from " << join_path;

            // Call __asmodeusJoin, then find and click the join button
            // with trusted input events (programmatic click is ignored by Meet)
            std::string call_js =
                "setTimeout(function() { if(window.__asmodeusJoin) {"
                "  window.__asmodeusJoin({agentName:'" + agent_name +
                "',displayName:'" + display + "'});"
                "} }, 1000);"
                // After join.js runs, find the "Ask to join" button and store its rect
                "setTimeout(function() {"
                "  var btns = document.querySelectorAll('button,[role=\\'button\\']');"
                "  for (var i = 0; i < btns.length; i++) {"
                "    var t = (btns[i].textContent||'').trim().toLowerCase();"
                "    if (t === 'ask to join' || t === 'join now' || t === 'join') {"
                "      var r = btns[i].getBoundingClientRect();"
                "      window.__asmodeusJoinBtnRect = {x: r.x + r.width/2, y: r.y + r.height/2};"
                "      document.title = '[ASMODEUS] BTN:' + JSON.stringify(window.__asmodeusJoinBtnRect);"
                "      break;"
                "    }"
                "  }"
                "}, 8000);";
            p->ExecuteJS(base::UTF8ToUTF16(call_js), base::NullCallback());
          },
          base::Unretained(raw_participant), pid, name, display_name),
      base::Seconds(5));

  // Post a second task to click "Ask to join" with a trusted user gesture.
  // ExecuteJavaScriptWithUserGestureForTests makes el.click() trusted.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](AsmodeusParticipant* p, std::string name) {
            if (!p || !p->is_loaded()) {
              LOG(ERROR) << "BrowserPlatform[" << name << "] not loaded";
              return;
            }
            auto* frame = p->web_contents()->GetPrimaryMainFrame();
            if (!frame || !frame->IsRenderFrameLive()) return;

            // Click the join button with a TRUSTED user gesture.
            frame->ExecuteJavaScriptWithUserGestureForTests(
                u"(function(){"
                u"var btns=document.querySelectorAll('button,[role=\"button\"]');"
                u"for(var i=0;i<btns.length;i++){"
                u"var t=(btns[i].textContent||'').trim().toLowerCase();"
                u"if(t==='ask to join'||t==='join now'||t==='join'){"
                u"btns[i].click();"
                u"document.title='[ASMODEUS] CLICKED: '+btns[i].textContent.trim();"
                u"break;"
                u"}"
                u"}"
                u"})();",
                base::NullCallback(),
                /*world_id=*/0);
            LOG(ERROR) << "BrowserPlatform[" << name
                       << "] executed trusted click on join button";
          },
          base::Unretained(raw_participant), name),
      base::Seconds(15));

  // Post a third task to log state after click.
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](AsmodeusParticipant* p, std::string name) {
            if (!p || !p->is_loaded()) return;
            std::string js =
                "JSON.stringify({title:document.title,url:window.location.href,"
                "btns:[].slice.call(document.querySelectorAll('button,[role=\\'button\\']'))"
                ".map(function(b){return b.textContent.trim()})"
                ".filter(function(t){return t.length>0&&t.length<30}).slice(0,10)})";
            p->ExecuteJS(base::UTF8ToUTF16(js),
                base::BindOnce([](const std::string& name, base::Value result) {
                  if (result.is_string()) {
                    LOG(ERROR) << "BrowserPlatform[" << name
                               << "] state after click: " << result.GetString();
                  }
                }, name));
          },
          base::Unretained(raw_participant), name),
      base::Seconds(30));

  LOG(ERROR) << "BrowserPlatform[" << platform_id_
             << "] agent '" << name << "' connecting to " << meeting_url_;
  return true;
}

void BrowserPlatform::DisconnectAgent(const std::string& name) {
  auto it = participants_.find(name);
  if (it != participants_.end()) {
    // Try to leave the meeting cleanly
    auto* participant = it->second.get();
    if (participant->is_loaded()) {
      participant->ExecuteJS(
          u"if(window.__asmodeusLeave) window.__asmodeusLeave();",
          base::NullCallback());
    }
    participants_.erase(it);
    LOG(INFO) << "BrowserPlatform[" << platform_id_
              << "] agent '" << name << "' disconnected";
  }
}

// static
std::string BrowserPlatform::DetectPlatform(const std::string& url) {
  if (url.find("meet.google.com") != std::string::npos) return "meet";
  if (url.find("teams.microsoft.com") != std::string::npos) return "teams";
  if (url.find("teams.live.com") != std::string::npos) return "teams";
  if (url.find("app.slack.com") != std::string::npos) return "slack";
  if (url.find("zoom.us") != std::string::npos) return "zoom";
  if (url.find("discord.com") != std::string::npos) return "discord";
  if (url.find("web.whatsapp.com") != std::string::npos) return "whatsapp";
  return "unknown";
}

}  // namespace asmodeus
