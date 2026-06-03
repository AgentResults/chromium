// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C5.f — audio handle (legion://chrome/browser/tabs/<id>/audio).
//
// RED-first: authored against the unbuilt AurelianAudioHandle.
// tell("mute", {muted}) mutes/unmutes; ask("isAudible") reports whether
// audio is currently playing. The speaker-silence check is an
// operator-presence SIT, refused in CI.

#include "aurelian/handles/browser/tab_handle.h"

#include "base/command_line.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "velite/agentspaces-wire/handle.hpp"
#include "velite/agentspaces-wire/value_handle.hpp"

namespace aurelian {

using V = velite::agentspaces::Value;
using StateKind = velite::agentspaces::StateKind;

class AurelianAudioBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    // Let Web Audio start without a user gesture so the audibility test can
    // produce real sound.
    command_line->AppendSwitchASCII("autoplay-policy",
                                    "no-user-gesture-required");
  }

  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  std::shared_ptr<velite::agentspaces::Handle>& GetHandle(
      TabHandleImpl* impl) {
    return *static_cast<std::shared_ptr<velite::agentspaces::Handle>*>(
        impl->handle_ptr);
  }

  bool AskBool(velite::agentspaces::Handle* audio, const std::string& verb) {
    auto r = audio->ask(verb, V());
    EXPECT_EQ(r->state_kind(), StateKind::ResolvedValue);
    return r->resolved_value().is_bool() && r->resolved_value().as_bool();
  }

  void PumpFor(base::TimeDelta delay) {
    base::RunLoop run_loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), delay);
    run_loop.Run();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianAudioBrowserTest, AudioHandleMounts) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>audio</body>")));
  auto tab = CreateTabHandle(GetWC(), 9501);
  auto& handle = GetHandle(tab.get());

  auto audio = handle->ask("audio", V());
  ASSERT_EQ(audio->state_kind(), StateKind::ResolvedValue);
  ASSERT_TRUE(audio->resolved_value().is_string());
  EXPECT_NE(audio->resolved_value().as_string().find(
                "legion://chrome/browser/tabs/9501/audio"),
            std::string::npos);

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianAudioBrowserTest, MuteRoundTrip) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>audio</body>")));
  auto tab = CreateTabHandle(GetWC(), 9502);
  auto& handle = GetHandle(tab.get());

  auto audio = handle->ask("audio", V());
  ASSERT_EQ(audio->state_kind(), StateKind::ResolvedValue);

  EXPECT_FALSE(AskBool(audio.get(), "isMuted"));
  audio->tell("mute", V::make_object({{"muted", V(true)}}));
  EXPECT_TRUE(AskBool(audio.get(), "isMuted"));
  EXPECT_TRUE(GetWC()->IsAudioMuted());
  audio->tell("mute", V::make_object({{"muted", V(false)}}));
  EXPECT_FALSE(AskBool(audio.get(), "isMuted"));

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianAudioBrowserTest, SilentPageNotAudible) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>quiet</body>")));
  auto tab = CreateTabHandle(GetWC(), 9503);
  auto& handle = GetHandle(tab.get());

  auto audio = handle->ask("audio", V());
  ASSERT_EQ(audio->state_kind(), StateKind::ResolvedValue);
  EXPECT_FALSE(AskBool(audio.get(), "isAudible"))
      << "a silent page is not audible";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianAudioBrowserTest, UnmutedPlaybackIsAudible) {
  // Start a Web Audio oscillator -> the tab produces real sound.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<body><script>"
           "const c=new AudioContext();"
           "const o=c.createOscillator();o.frequency.value=440;"
           "o.connect(c.destination);o.start();window.__c=c;"
           "</script></body>")));
  auto tab = CreateTabHandle(GetWC(), 9504);
  auto& handle = GetHandle(tab.get());

  auto audio = handle->ask("audio", V());
  ASSERT_EQ(audio->state_kind(), StateKind::ResolvedValue);

  // Audibility detection is asynchronous; poll until the tab is audible.
  bool audible = false;
  for (int i = 0; i < 50 && !audible; ++i) {
    audible = GetWC()->IsCurrentlyAudible();
    if (!audible) PumpFor(base::Milliseconds(100));
  }
  EXPECT_TRUE(audible) << "oscillator playback should make the tab audible";
  EXPECT_TRUE(AskBool(audio.get(), "isAudible"))
      << "isAudible must report the playing tab";

  DestroyTabHandle(std::move(tab));
}

IN_PROC_BROWSER_TEST_F(AurelianAudioBrowserTest, SpeakerSilenceRequiresOperator) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>audio</body>")));
  auto tab = CreateTabHandle(GetWC(), 9505);
  auto& handle = GetHandle(tab.get());

  auto audio = handle->ask("audio", V());
  ASSERT_EQ(audio->state_kind(), StateKind::ResolvedValue);

  // Confirming actual speaker silence needs a human in the room; refused
  // in CI (operator-presence SIT).
  auto check = audio->ask("verifySpeakerSilence", V());
  ASSERT_EQ(check->state_kind(), StateKind::Broken);
  EXPECT_EQ(std::string(check->broken_reason()), "requires-operator-presence");

  DestroyTabHandle(std::move(tab));
}

}  // namespace aurelian
