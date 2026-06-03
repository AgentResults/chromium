// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C6.b — Mojo Subscribe + VeliteSink cross-process round-trip.
//
// RED-first: the renderer's Subscribe binds the VeliteSink but does not
// emit frames yet, so CrossProcessFrameDelivery times out with 0 frames.
// Starting the producer makes it GREEN.

#include "aurelian/handles/streams/mojo_stream_bridge.h"

#include <memory>
#include <string>
#include <vector>

#include "aurelian/public/mojom/aurelian_wire.mojom.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianStreamBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  content::RenderFrameHost* MainFrame() {
    return GetWC()->GetPrimaryMainFrame();
  }

  void GetWire(mojo::AssociatedRemote<mojom::AurelianWire>* wire) {
    MainFrame()->GetRemoteAssociatedInterfaces()->GetInterface(wire);
  }

  // Spin the loop until `cond` is true or timeout.
  void PumpUntil(base::RepeatingCallback<bool()> cond, base::TimeDelta limit) {
    if (cond.Run()) return;
    base::RunLoop run_loop;
    base::RepeatingTimer timer;
    timer.Start(FROM_HERE, base::Milliseconds(10),
                base::BindRepeating(
                    [](base::RunLoop* loop, base::RepeatingCallback<bool()> c) {
                      if (c.Run()) loop->Quit();
                    },
                    &run_loop, cond));
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), limit);
    run_loop.Run();
    timer.Stop();
  }

  void PumpFor(base::TimeDelta delay) {
    base::RunLoop run_loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, run_loop.QuitClosure(), delay);
    run_loop.Run();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianStreamBrowserTest, CrossProcessFrameDelivery) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>stream</body>")));

  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);

  std::vector<std::string> frames;
  MojoStreamBridge bridge(base::BindRepeating(
      [](std::vector<std::string>* f, const std::vector<uint8_t>& bytes) {
        f->emplace_back(bytes.begin(), bytes.end());
      },
      &frames));
  bridge.Subscribe(wire, "test");

  PumpUntil(base::BindLambdaForTesting(
                [&]() { return frames.size() >= 3u; }),
            base::Seconds(10));

  ASSERT_GE(frames.size(), 3u) << "renderer frames never arrived";
  EXPECT_EQ(frames[0], "tick-0");
  EXPECT_EQ(frames[1], "tick-1");
  EXPECT_EQ(frames[2], "tick-2");
}

IN_PROC_BROWSER_TEST_F(AurelianStreamBrowserTest, CancelStopsFrames) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>stream</body>")));

  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);

  std::vector<std::string> frames;
  auto bridge = std::make_unique<MojoStreamBridge>(base::BindRepeating(
      [](std::vector<std::string>* f, const std::vector<uint8_t>& bytes) {
        f->emplace_back(bytes.begin(), bytes.end());
      },
      &frames));
  bridge->Subscribe(wire, "test");

  PumpUntil(base::BindLambdaForTesting(
                [&]() { return frames.size() >= 2u; }),
            base::Seconds(10));
  ASSERT_GE(frames.size(), 2u);

  // Cancel by dropping the sink; no further frames should be delivered.
  size_t count = frames.size();
  bridge.reset();
  PumpFor(base::Milliseconds(300));
  EXPECT_EQ(frames.size(), count) << "frames kept arriving after cancel";
}

// C6.c — the renderer /dom/mutations producer (MutationObserver) delivers
// a frame per DOM mutation over Mojo.
IN_PROC_BROWSER_TEST_F(AurelianStreamBrowserTest, ReceivesMutations) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("data:text/html,<body><div id='host'>m</div></body>")));

  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);

  std::vector<std::string> frames;
  MojoStreamBridge bridge(base::BindRepeating(
      [](std::vector<std::string>* f, const std::vector<uint8_t>& bytes) {
        f->emplace_back(bytes.begin(), bytes.end());
      },
      &frames));
  bridge.Subscribe(wire, "mutations");

  // Wait until the renderer has installed the MutationObserver (it does so in
  // Subscribe, before replying), so the mutation below cannot race it.
  EXPECT_EQ(true,
            content::EvalJs(
                GetWC(),
                "(async()=>{for(let i=0;i<200;i++){if(window.__aurelian_mut_init)"
                "return true;await new Promise(r=>setTimeout(r,10));}"
                "return false;})()")
                .ExtractBool());

  // Mutate the DOM: an attribute change and a node insertion.
  ASSERT_TRUE(content::ExecJs(
      GetWC(),
      "document.getElementById('host').setAttribute('data-x','1');"
      "document.body.appendChild(document.createElement('span'));"));

  PumpUntil(base::BindLambdaForTesting([&]() { return !frames.empty(); }),
            base::Seconds(10));
  ASSERT_FALSE(frames.empty()) << "no mutation frames arrived";

  bool found_attr = false;
  bool found_child = false;
  for (const auto& f : frames) {
    if (f.find("attributes") != std::string::npos) found_attr = true;
    if (f.find("childList") != std::string::npos) found_child = true;
  }
  EXPECT_TRUE(found_attr || found_child)
      << "expected an attributes/childList mutation frame";
}

// C6.d — the renderer /console producer (console.* interceptor) delivers a
// frame per console message over Mojo, carrying the level + text.
IN_PROC_BROWSER_TEST_F(AurelianStreamBrowserTest, ReceivesConsole) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("data:text/html,<body>console</body>")));

  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  GetWire(&wire);

  std::vector<std::string> frames;
  MojoStreamBridge bridge(base::BindRepeating(
      [](std::vector<std::string>* f, const std::vector<uint8_t>& bytes) {
        f->emplace_back(bytes.begin(), bytes.end());
      },
      &frames));
  bridge.Subscribe(wire, "console");

  // Wait until the renderer has installed the console interceptor (it does so
  // in Subscribe, before replying), so the log below cannot race it.
  EXPECT_EQ(true,
            content::EvalJs(
                GetWC(),
                "(async()=>{for(let i=0;i<200;i++){"
                "if(window.__aurelian_console_init)return true;"
                "await new Promise(r=>setTimeout(r,10));}return false;})()")
                .ExtractBool());

  // Emit console messages at two levels.
  ASSERT_TRUE(content::ExecJs(
      GetWC(),
      "console.log('hello-aurelian-console', 42);"
      "console.warn('a-warning');"));

  PumpUntil(base::BindLambdaForTesting([&]() { return frames.size() >= 2u; }),
            base::Seconds(10));
  ASSERT_GE(frames.size(), 2u) << "no console frames arrived";

  bool found_log = false;
  bool found_warn = false;
  for (const auto& f : frames) {
    if (f.find("hello-aurelian-console") != std::string::npos &&
        f.find("\"log\"") != std::string::npos) {
      found_log = true;
    }
    if (f.find("a-warning") != std::string::npos &&
        f.find("\"warn\"") != std::string::npos) {
      found_warn = true;
    }
  }
  EXPECT_TRUE(found_log) << "expected a log-level frame carrying the message";
  EXPECT_TRUE(found_warn) << "expected a warn-level frame carrying the message";
}

}  // namespace aurelian
