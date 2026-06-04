// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C-MEDIA-2a (RED-first): the avatar appears as a REAL browser camera.
// AurelianVirtualCamera registers a virtual device with Chromium's video
// capture service; a real web page's navigator.mediaDevices.enumerateDevices()
// must then list it as a videoinput. With fake real-devices off (device-count=0)
// the ONLY videoinput is the Aurelian one, so the count goes 0 -> 1 exactly when
// the device registers. RED before AddSharedMemoryVirtualDevice; GREEN after.
// Real service, real renderer, no mocks.

#include "aurelian/media/aurelian_virtual_camera.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "media/base/media_switches.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"

namespace aurelian {
namespace {

std::unique_ptr<net::test_server::HttpResponse> HandleRequest(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/html");
  response->set_content("<html><body>aurelian-camera-host</body></html>");
  return response;
}

// Counts videoinput devices the page can enumerate. Device count (not labels)
// is exposed without a permission grant, so this needs no getUserMedia open.
constexpr char kCountVideoInputs[] = R"((async () => {
  const devices = await navigator.mediaDevices.enumerateDevices();
  return devices.filter(d => d.kind === 'videoinput').length;
})())";

}  // namespace

class AurelianVirtualCameraBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    // No real or fake hardware cameras: the only videoinput the page can see is
    // the Aurelian virtual device we register.
    command_line->AppendSwitchASCII(switches::kUseFakeDeviceForMediaStream,
                                    "device-count=0");
    command_line->AppendSwitch(switches::kUseFakeUIForMediaStream);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(
        base::BindRepeating(&HandleRequest));
    ASSERT_TRUE(embedded_test_server()->Start());
  }
};

IN_PROC_BROWSER_TEST_F(AurelianVirtualCameraBrowserTest,
                       AvatarAppearsAsEnumerableCamera) {
  // A real (localhost = secure-context) page.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/cam.html")));
  content::WebContents* wc =
      browser()->tab_strip_model()->GetActiveWebContents();

  // Before registration the page sees zero cameras.
  EXPECT_EQ(0, content::EvalJs(wc, kCountVideoInputs));

  // Register the Aurelian avatar device with the real video capture service.
  AurelianVirtualCamera camera;
  camera.Start();
  ASSERT_TRUE(camera.registered());
  camera.FlushForTesting();

  // The page now enumerates exactly one videoinput: the Aurelian camera.
  EXPECT_EQ(1, content::EvalJs(wc, kCountVideoInputs));
}

}  // namespace aurelian
