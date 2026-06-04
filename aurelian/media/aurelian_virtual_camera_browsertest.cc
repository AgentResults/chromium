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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aurelian/handles/media/media_seam.h"
#include "base/functional/bind.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "media/base/media_switches.h"
#include "media/capture/capture_switches.h"
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

// Opens the (only) camera via getUserMedia, waits for a presented frame, draws
// it to a canvas, and returns the average brightness of the centre pixel (or -1
// on any timeout / no frame). Every await is bounded so the script ALWAYS
// resolves to a number — an unfed device (whose getUserMedia never starts)
// yields -1 cleanly rather than hanging the harness.
constexpr char kAvgBrightnessOfFirstFrame[] = R"((async () => {
  const withTimeout = (p, ms) => Promise.race(
      [p, new Promise((res) => setTimeout(() => res('__t__'), ms))]);
  let stream;
  try {
    stream = await withTimeout(
        navigator.mediaDevices.getUserMedia({video: true}), 7000);
  } catch (e) { return -1; }
  if (stream === '__t__') return -1;
  const video = document.createElement('video');
  video.srcObject = stream;
  video.muted = true;
  video.playsInline = true;
  try { await withTimeout(video.play(), 3000); } catch (e) {}
  const px = await new Promise((resolve) => {
    let done = false;
    const timer = setTimeout(() => { if (!done) { done = true; resolve(null); } },
                             7000);
    const onFrame = () => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      const c = document.createElement('canvas');
      c.width = video.videoWidth || 640;
      c.height = video.videoHeight || 480;
      const ctx = c.getContext('2d');
      ctx.drawImage(video, 0, 0, c.width, c.height);
      const d = ctx.getImageData(c.width >> 1, c.height >> 1, 1, 1).data;
      resolve([d[0], d[1], d[2]]);
    };
    if ('requestVideoFrameCallback' in HTMLVideoElement.prototype) {
      video.requestVideoFrameCallback(onFrame);
    } else {
      setTimeout(onFrame, 2000);
    }
  });
  if (stream.getTracks) stream.getTracks().forEach(t => t.stop());
  if (!px) return -1;
  return Math.round((px[0] + px[1] + px[2]) / 3);
})())";

// A 640x480 I420 frame with the given luma (U/V neutral) — a flat "avatar".
std::vector<uint8_t> MakeI420Frame(uint8_t luma) {
  const size_t y_size = 640u * 480u;
  const size_t uv_size = (640u / 2) * (480u / 2);
  std::vector<uint8_t> pixels(y_size + 2 * uv_size, 128);
  std::fill(pixels.begin(), pixels.begin() + y_size, luma);
  return pixels;
}

}  // namespace

class AurelianVirtualCameraBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    // No real or fake hardware cameras: the only videoinput the page can see is
    // the Aurelian virtual device we register.
    command_line->AppendSwitchASCII(switches::kUseFakeDeviceForMediaStream,
                                    "device-count=0");
    command_line->AppendSwitch(switches::kUseFakeUIForMediaStream);
    // SharedMemoryVirtualDevice frames are shmem-backed; force the consumer
    // buffer type to shmem too (GpuMemoryBuffer is default on Mac). This is the
    // sanctioned config for virtual devices (see
    // content/browser/webrtc/webrtc_video_capture_shared_device_browsertest).
    command_line->AppendSwitch(switches::kDisableVideoCaptureUseGpuMemoryBuffer);
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

// C-MEDIA-2b: the avatar PIXELS flow through the camera. A bright avatar frame
// in MediaSeam must reach a getUserMedia track — the page renders the camera to
// a canvas and reads a bright centre pixel. RED before the frame pump (no frame
// presented -> -1); GREEN once the pump fills service buffers from MediaSeam.
IN_PROC_BROWSER_TEST_F(AurelianVirtualCameraBrowserTest,
                       AvatarPixelsFlowThroughCamera) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/cam.html")));
  content::WebContents* wc =
      browser()->tab_strip_model()->GetActiveWebContents();

  // The avatar paints a bright frame (luma 220) into the seam BEFORE the camera
  // starts, so the very first pumped frame already carries it.
  MediaSeam::Get().PushVideoFrame(
      {640, 480, "I420", MakeI420Frame(/*luma=*/220), /*ts_micros=*/0});

  AurelianVirtualCamera camera;
  camera.Start();
  ASSERT_TRUE(camera.registered());

  // getUserMedia opens the Aurelian camera; the rendered centre pixel is bright
  // (the avatar luma), not black/absent. Threshold 150 separates the avatar
  // (~220) from the neutral-gray fallback (128) and from no-frame (-1).
  int brightness = content::EvalJs(wc, kAvgBrightnessOfFirstFrame).ExtractInt();
  EXPECT_GT(brightness, 150) << "rendered camera brightness = " << brightness;
}

}  // namespace aurelian
