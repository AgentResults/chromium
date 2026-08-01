// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C10 — asmodeus tab-audio capture, now routed through a Velite handle:
// start / level / stop flow through the handle's ask dispatch on a real tab.
// AU-AUDIO-LEVEL: the capture must not merely *read* a level — it must observe
// REAL audio energy. The boot-wired Aurelian virtual mic is fed a loud TTS
// square wave through MediaSeam (the same proven source the virtual-camera SIT
// uses); the captured tab routes it into an <audio> element the asmodeus
// capture hooks. After a capture window the handle's reported peak rms MUST be
// > 0 — a silent (or non-bridged) capture reports 0, which is the discriminator
// the previous test never asserted.

#include "aurelian/handles/profile/audio_capture_handle.h"

#include <cstring>
#include <vector>

#include "aurelian/handles/media/media_seam.h"
#include "base/command_line.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "media/base/media_switches.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace aurelian {
namespace {

// Full-scale ~1kHz square wave (RMS ~0.9) — a loud "TTS" signal far above any
// default fake-mic beep. float32 LE, the mic contract pcm bytes.
std::vector<uint8_t> MakeSquareWave(size_t samples) {
  std::vector<float> wave(samples);
  for (size_t i = 0; i < samples; ++i) {
    wave[i] = ((i / 24) % 2 == 0) ? 0.9f : -0.9f;  // 48k/48 = 1kHz
  }
  std::vector<uint8_t> bytes(wave.size() * sizeof(float));
  std::memcpy(bytes.data(), wave.data(), bytes.size());
  return bytes;
}

}  // namespace

class AurelianAudioHandleBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    // No standard fake devices, so getUserMedia({audio}) resolves to the
    // boot-wired Aurelian virtual mic (fed by MediaSeam). Auto-grant the
    // permission; allow the asmodeus AudioContext to resume without a gesture.
    command_line->AppendSwitchASCII(switches::kUseFakeDeviceForMediaStream,
                                    "device-count=0");
    // SELECT the virtual mic. Without this, GetDefaultInputDeviceID() returns
    // the REAL hardware input, and with device-count=0 there is nothing to
    // open, so getUserMedia({audio}) hangs until the test times out. The flag
    // makes the Aurelian ring both the enumerated device
    // (audio_manager_mac :: AsmodeusDiscoverDevices) and the default input, and
    // AurelianVirtualMic::DefaultShmPath() now honours the same flag so the
    // producer writes the file the reader opens.
    command_line->AppendSwitchASCII(switches::kAsmodeusDevice, "aurelian");
    command_line->AppendSwitch(switches::kUseFakeUIForMediaStream);
    command_line->AppendSwitchASCII("autoplay-policy",
                                    "no-user-gesture-required");
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  content::WebContents* GetWC() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(AurelianAudioHandleBrowserTest, StartLevelStop) {
  // A 127.0.0.1 origin is a secure context, so navigator.mediaDevices (and the
  // boot-wired virtual mic) are available — a data: URL is not.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), embedded_test_server()->GetURL("/title1.html")));
  content::WebContents* wc = GetWC();

  // Open the virtual mic and route it into an <audio> element BEFORE capture
  // starts, so asmodeus's querySelectorAll('audio') hooks its live srcObject.
  ASSERT_EQ(1, content::EvalJs(wc, R"JS(
    (async () => {
      const s = await navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: false, noiseSuppression: false,
                 autoGainControl: false } });
      const a = document.createElement('audio');
      a.srcObject = s;
      a.muted = true;          // tap the stream, not the speaker
      a.id = 'cap';
      document.body.appendChild(a);
      try { await a.play(); } catch (e) {}
      window.__capStream = s;  // keep it alive
      return s.getAudioTracks().length;
    })()
  )JS"));

  // The capture (and temp dir) do blocking WAV file I/O, which the asmodeus
  // path performs on this thread; allow it for the test.
  base::ScopedAllowBlockingForTesting allow_blocking;

  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  std::string wav = dir.GetPath().AppendASCII("cap.wav").AsUTF8Unsafe();

  ScopedAudioCaptureSession session(CreateAudioCaptureHandle(wc));
  ASSERT_NE(session.get(), nullptr);

  // Before start: not capturing.
  double rms = -1, peak = -1;
  bool capturing = true;
  AudioCaptureHandleLevel(session.get(), &rms, &peak, &capturing);
  EXPECT_FALSE(capturing);

  // Drain ~2.5s of full-scale TTS into the seam; the boot mic pump fills the
  // ring the virtual mic reads (covering the capture window below).
  MediaSeam::Get().PushAudioFrame(MakeSquareWave(/*samples=*/120000));

  // Start routed through the handle.
  ASSERT_TRUE(AudioCaptureHandleStart(session.get(), wav, 48000, 1));

  // Now capturing — read through the handle.
  AudioCaptureHandleLevel(session.get(), &rms, &peak, &capturing);
  EXPECT_TRUE(capturing);

  // Let the asmodeus ScriptProcessor accumulate ~1.5s of the loud tone.
  {
    base::RunLoop loop;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, loop.QuitClosure(), base::Milliseconds(1500));
    loop.Run();
  }

  // Stop routed through the handle; the WAV path round-trips AND the observed
  // peak level is non-zero — the capture genuinely saw audio energy. A silent
  // (or non-bridged) capture reports 0, so this assertion is the discriminator.
  AudioCaptureStats stats = AudioCaptureHandleStop(session.get());
  EXPECT_EQ(stats.output_path, wav);
  EXPECT_GT(stats.peak_rms, 0.0)
      << "captured peak rms = " << stats.peak_rms
      << " (expected > 0 from the virtual-mic TTS tone)";

  // After stop: not capturing.
  AudioCaptureHandleLevel(session.get(), &rms, &peak, &capturing);
  EXPECT_FALSE(capturing);
}

}  // namespace aurelian
