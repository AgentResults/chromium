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
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "url/gurl.h"

namespace aurelian {

class AurelianStreamBrowserTest : public InProcessBrowserTest {
 protected:
  content::RenderFrameHost* MainFrame() {
    return browser()
        ->tab_strip_model()
        ->GetActiveWebContents()
        ->GetPrimaryMainFrame();
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

}  // namespace aurelian
