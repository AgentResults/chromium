# Native Agent — Quality Fixes Plan

## Issues to Fix (from critical review)

### 1. Extract NativeAgent class from main.cc
**Problem**: main.cc is 511 lines with Speak(), VideoShmHeader, CreateVideoShm, WriteFrameToShm, Resample, SavePpm all in anonymous namespace. Command handler is a 50-line lambda.
**Fix**: Create `native_agent.h/cc` class that owns all state. main.cc becomes ~50 lines.
**Test**: Agent starts, speaks, shuts down — same behavior as before.

### 2. Fix thread safety
**Problem**: `speaking`, `peer_manager` accessed from signaling IO thread without sync. Signaling callbacks fire on IO thread but touch main-thread state.
**Fix**: All signaling callbacks PostTask to main thread. Use `base::SequencedTaskRunner` for main thread. Agent state only touched on main sequence.
**Test**: Two agents connect via signaling + WebRTC — no races, ICE reaches completed.

### 3. Speak on background thread (non-blocking)
**Problem**: `speak` command blocks the control channel IO thread for 3-4 seconds.
**Fix**: `speak` posts TTS work to `base::ThreadPool`. Returns `{"ok":true,"queued":true}` immediately. Emits `speech_started` when TTS begins, `speech_ended` when done.
**Test**: Send `speak` + `get_state` rapidly — get_state responds while speech is in progress.

### 4. Remove goto cleanup
**Problem**: `goto cleanup` in main.cc.
**Fix**: Use NativeAgent RAII destructor for cleanup. main.cc just creates agent and runs.

### 5. Add tracing
**Problem**: Design specifies trace_event but none implemented.
**Fix**: Add `TRACE_EVENT` to Speak, avatar render, WebRTC connection, signaling events.
**Test**: Run with `--trace-file`, verify JSON contains expected trace events.

### 6. Thread-safe SignalingClient::Send
**Problem**: `Send()` writes to socket directly from any thread. `ReadLoop` reads on IO thread.
**Fix**: `Send()` posts write to IO thread via task_runner.
**Test**: Send ICE candidates from WebRTC thread while signaling IO thread reads — no corruption.

### 7. Proper JSON building
**Problem**: JSON built by string concatenation, no escaping.
**Fix**: Use `base::Value::Dict` + `base::JSONWriter::Write()` for all JSON output.
**Test**: Speak text with special characters (quotes, backslashes) — JSON is valid.

### 8. Deduplicate VideoShmHeader
**Problem**: Defined in both main.cc and shm_video_source.cc.
**Fix**: Move to a shared header `video_shm.h`.

## Implementation Order

1. Create `video_shm.h` (shared VideoShmHeader) — trivial
2. Create `native_agent.h/cc` — extract from main.cc
3. Fix JSON building — use base::Value everywhere
4. Fix SignalingClient::Send thread safety — post to IO thread
5. Fix speak to run on background thread
6. Add tracing
7. Test everything end-to-end

## Test Plan

After each fix, verify:
```bash
# Build
cd chromium/src && third_party/ninja/ninja -C out/Default asmodeus_agent

# Test 1: Agent starts and speaks via --speak flag
./out/Default/asmodeus_agent --name=alice --speak="Hello world"

# Test 2: Two agents with control channel
./out/Default/asmodeus_agent --name=carol --control-port=9610 &
./out/Default/asmodeus_agent --name=dave --control-port=9611 &
# Send speak commands via WebSocket, verify responses

# Test 3: Two agents with signaling + WebRTC (requires Chrome signaling server)
# Verify ICE reaches completed, audio tracks connected

# Test 4: Non-blocking speak
# Send speak + get_state rapidly, verify get_state responds while speaking
```
