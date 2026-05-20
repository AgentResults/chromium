// Standalone test for MeetingCoordinator.
// Build: add to asmodeus BUILD.gn as executable("meeting_coordinator_test")
// Run: ./out/Default/meeting_coordinator_test

#include <csignal>
#include <cstdio>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/message_loop/message_pump_type.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/platform_thread.h"
#include "chrome/browser/asmodeus/meeting_coordinator.h"

int main(int argc, char* argv[]) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  logging::SetMinLogLevel(logging::LOGGING_INFO);

  base::SingleThreadTaskExecutor main_executor(base::MessagePumpType::DEFAULT);
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("test");

  LOG(INFO) << "=== Meeting Coordinator Test ===";

  std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
  std::string agent_path = home + "/workspace/chromium/src/out/Default/asmodeus_agent";
  std::string voice = home + "/.asmodeus/voices/en_US-kristin-medium.onnx";
  std::string voice2 = home + "/.asmodeus/voices/en_US-joe-medium.onnx";
  std::string output_dir = "/tmp/meeting-coord-test";

  asmodeus::MeetingCoordinator coordinator;

  // Step 1: Start meeting
  LOG(INFO) << "--- Step 1: Start meeting ---";
  if (!coordinator.Start("Test Meeting", agent_path)) {
    LOG(ERROR) << "FAILED: Start meeting";
    return 1;
  }
  LOG(INFO) << "Meeting started, signaling port=" << coordinator.signaling_port();

  // Step 2: Add agents
  LOG(INFO) << "--- Step 2: Add agents ---";
  if (!coordinator.AddAgent("alice", voice, "Alice")) {
    LOG(ERROR) << "FAILED: Add alice";
    return 1;
  }
  if (!coordinator.AddAgent("bob", voice2, "Bob")) {
    LOG(ERROR) << "FAILED: Add bob";
    return 1;
  }
  LOG(INFO) << "Agents added: " << coordinator.agents().size();

  // Step 3: Render a frame (compositor test)
  LOG(INFO) << "--- Step 3: Render frame ---";
  SkBitmap frame = coordinator.RenderFrame();
  LOG(INFO) << "Frame rendered: " << frame.width() << "x" << frame.height();

  // Step 4: Start recording
  LOG(INFO) << "--- Step 4: Start recording ---";
  system(("mkdir -p " + output_dir).c_str());
  if (!coordinator.StartRecording(output_dir)) {
    LOG(ERROR) << "FAILED: Start recording";
    return 1;
  }

  // Step 5: Speak
  LOG(INFO) << "--- Step 5: Alice speaks ---";
  coordinator.Speak("alice", "Hello Bob, welcome to the meeting.");
  base::PlatformThread::Sleep(base::Seconds(5));

  LOG(INFO) << "--- Step 6: Bob speaks ---";
  coordinator.Speak("bob", "Thank you Alice, great to be here.");
  base::PlatformThread::Sleep(base::Seconds(5));

  // Step 7: Stop recording
  LOG(INFO) << "--- Step 7: Stop recording ---";
  int frames = 0;
  int64_t samples = 0;
  double peak_rms = 0;
  std::string mp4_path;
  coordinator.StopRecording(&frames, &samples, &peak_rms, &mp4_path);
  LOG(INFO) << "Recording: " << frames << " frames, " << samples
            << " samples, peak_rms=" << peak_rms;
  LOG(INFO) << "MP4: " << mp4_path;

  // Step 8: Verify
  LOG(INFO) << "--- Step 8: Verify ---";
  LOG(INFO) << "Frames > 50: " << (frames > 50 ? "PASS" : "FAIL");
  LOG(INFO) << "Samples > 0: " << (samples > 0 ? "PASS" : "FAIL");

  // Cleanup
  LOG(INFO) << "--- Cleanup ---";
  coordinator.Stop();

  base::ThreadPoolInstance::Get()->Shutdown();
  LOG(INFO) << "=== Test Complete ===";
  return 0;
}
