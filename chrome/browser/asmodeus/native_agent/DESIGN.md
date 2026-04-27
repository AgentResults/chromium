# Asmodeus Native Agent — Design (v4)

## What This Is

A standalone C++ process that participates in meetings as a fully autonomous AI agent. No Chrome browser. No DOM. No renderer. The agent:

- **Listens** — receives audio from peers via WebRTC, runs VAD to detect speech, runs STT (whisper.cpp) to transcribe
- **Thinks** — sends transcription to LLM (Anthropic API via libcurl), generates a response with streaming sentence output
- **Speaks** — runs TTS (Piper) on each sentence as it arrives, streams audio to shm
- **Shows** — renders a lip-synced avatar (Skia) in lockstep with TTS audio
- **Cancels** — barge-in: if someone speaks while agent is talking, agent stops and listens
- **Echoes nothing** — AEC (SpeexDSP) removes the agent's own TTS from the captured mic signal

This is NOT a puppet. The agent autonomously participates in conversations. The `speak` control command is still available for scripted scenarios, but the default mode is **autonomous conversation**.

## Architecture

```
asmodeus_agent (standalone binary)
  │
  ├── ConversationEngine (thin orchestrator wiring three layers)
  │     States: Idle → Listening → Thinking → Speaking → Listening
  │     Connects: AudioPipeline → DialogueManager → SpeechOutput
  │
  │     ┌── Layer 1: AudioPipeline (signal processing)
  │     │     AudioLoop thread: reads audio-out shm → AEC → VAD → STT
  │     │     Produces: UtteranceEvent {text, stt_ms, timestamp}
  │     │     Also: barge-in detection, input RMS monitoring
  │     │     Knows nothing about dialogue, LLM, TTS, or avatars
  │     │
  │     ├── Layer 2: DialogueManager (conversation logic)
  │     │     Owns: labeled transcript, participant list, TurnPolicy
  │     │     Input: UtteranceEvent → Output: DialogueAction (RESPOND/SILENT)
  │     │     TurnPolicy implementations: AlwaysRespond, AdjacencyPair, LlmTurn
  │     │     LlmTurnPolicy: single LLM call combines think + respond
  │     │     Knows nothing about audio, shm, WebRTC, TTS, or avatars
  │     │
  │     └── Layer 3: SpeechOutput (speech + avatar)
  │           TTS (Piper), avatar rendering (Skia), shm writing
  │           Input: text to speak → Output: audio + video in shm
  │           Knows nothing about dialogue, STT, VAD, or turn-taking
  │
  ├── Audio Input Path
  │     WebRTC playout writes mixed peer audio → audio-out-{name}.shm
  │     AudioLoop reads from audio-out shm (lock-free SPSC ring buffer)
  │     48kHz → 16kHz downsampler for VAD/STT/AEC pipeline
  │     No mutex, no FeedAudio() — AudioLoop reads shm directly
  │
  ├── VAD (Silero ONNX, 16kHz)
  │     512-sample frames (32ms at 16kHz)
  │     Detects speech start/end events
  │     Configurable: threshold, min_speech_ms, min_silence_ms
  │
  ├── AEC (SpeexDSP, 16kHz)
  │     Removes own TTS from captured audio
  │     Reference signal: TTS output downsampled 22050→16kHz
  │     Capture signal: peer audio downsampled 48k→16k
  │     Output: clean peer-only audio for STT
  │
  ├── STT (whisper.cpp, 16kHz)
  │     Fed continuously with AEC output
  │     Transcribes on SpeechEnd event (< 200ms on Apple Silicon with Metal)
  │     Reset after each transcription
  │     No speaker attribution — receives mixed peer audio
  │
  ├── LLM (Anthropic API, streaming)
  │     libcurl SSE client
  │     Streams tokens, splits into sentences
  │     Each sentence → immediate TTS (low latency pipeline)
  │     Abortable (for barge-in)
  │     Personality prompt configurable via --personality flag
  │     Conversation history: sliding window, last --max-history turns (default 20)
  │     API key from ANTHROPIC_API_KEY env var only (never a CLI flag)
  │
  ├── TTS Engine (Piper) — owned by ConversationEngine
  │     Piper binary: text → PCM at 22050Hz
  │     Resampled to 48kHz via linear interpolation
  │     Streamed to audio-in-{name}.shm in 20ms chunks
  │     Each chunk: resample → write shm → compute RMS → render avatar → write video shm
  │     Same code path for autonomous (LLM→TTS) and scripted (speak command→TTS)
  │
  ├── Avatar Renderer (Skia) — owned by ConversationEngine
  │     Renders lip-synced face frames (640x480)
  │     Input: RMS amplitude from current TTS audio chunk
  │     Deterministic appearance per agent (skin, hair, eyes from name hash)
  │     Produces RGBA → NV12 via libyuv → video-in-{name}.shm
  │     Called only from SpeechWorker thread (no cross-thread Skia calls)
  │     Idle frames rendered by a 1fps timer on main thread (stopped during speech)
  │
  ├── Shared Memory (IPC with coordinator)
  │     audio-in-{name}.shm  — agent writes TTS audio (read by coordinator + WebRTC ADM)
  │     audio-out-{name}.shm — WebRTC playout writes peer audio (read by AudioLoop)
  │     video-in-{name}.shm  — avatar frames (read by coordinator + WebRTC video source)
  │
  ├── WebRTC (native, no browser)
  │     PeerConnectionFactory with ShmAudioDeviceModule
  │     ShmAudioDeviceModule handles:
  │       Capture: reads audio-in shm → sends our TTS to peers
  │       Playout: receives mixed peer audio → writes to audio-out shm
  │     ShmVideoSource: reads video-in shm → sends our avatar to peers
  │
  ├── Signaling Client
  │     Raw WebSocket client (HTTP upgrade + frame parsing)
  │     Connects to AsmodeusMeetingServer
  │
  ├── Control Channel (net::HttpServer WebSocket)
  │     Commands: speak, get_state, set_mode, mute, unmute, set_voice, shutdown
  │     Modes: autonomous (default), scripted (speak-only, no STT/LLM)
  │     Events: speech_started, speech_ended, heard, thinking, peer_connected, etc.
  │
  └── Tracing (base/trace_event)
        Every operation traced with category "asmodeus"
```

## Conversation Pipeline — Detailed Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ AudioLoop Thread (continuous, never blocks on state)            │
│                                                                 │
│  Read audio-out shm directly (lock-free SPSC, 480 samples @48k)│
│       │                                                         │
│       ▼                                                         │
│  Downsample 48k → 16k (160 samples)                            │
│       │                                                         │
│       ▼                                                         │
│  AEC: remove own TTS echo                                       │
│       │                                                         │
│       ▼                                                         │
│  Feed to Whisper STT (continuous accumulation)                  │
│       │                                                         │
│       ▼                                                         │
│  Feed to Silero VAD (512-sample chunks)                         │
│       │                                                         │
│       ├── SpeechStart + agent is Speaking → BARGE-IN            │
│       │     Cancel LLM, cancel TTS, reset STT                  │
│       │     State → Listening                                   │
│       │                                                         │
│       └── SpeechEnd + agent is Listening → TURN DETECTED        │
│             Transcribe (Whisper, <200ms)                         │
│             Add to transcript: {speaker, text, timestamp}        │
│             If text.length >= 3:                                 │
│               TurnPolicy.Evaluate(transcript, text, my_name)    │
│               If RESPOND → spawn SpeechWorker (with response)   │
│               If SILENT → log reason, stay listening             │
│             Reset STT buffer                                    │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│ SpeechWorker Thread (spawned per turn, owns all TTS+avatar I/O) │
│                                                                 │
│  State → Thinking                                               │
│  user_text → LLM.Generate(streaming)                            │
│       │                                                         │
│       ├── on_first_token → State → Speaking                     │
│       │                                                         │
│       ├── on_sentence("sentence text") →                        │
│       │     TTS.Synthesize(sentence, chunk_callback)            │
│       │       chunk_callback(samples_22k, n):                   │
│       │         Resample 22k → 48k → Write audio-in shm        │
│       │         Resample 22k → 16k → AEC reference             │
│       │         Compute RMS → jaw_open → avatar_renderer_.Render│
│       │         Convert RGBA→NV12 → Write video-in shm         │
│       │                                                         │
│       ├── on_complete(full_text) → save to history              │
│       │                                                         │
│       └── cancel_speech_ flag → abort LLM + TTS immediately    │
│                                                                 │
│  State → Listening                                              │
└─────────────────────────────────────────────────────────────────┘

For scripted speak (control command or CDP):
  Same SpeechWorker, but skip LLM — go directly to TTS with the given text.
  is_intro=true → no LLM call, just synthesize and stream.
```

## Threading Model

```
Main thread (base::MessageLoop)
  ├── Owns NativeAgent state, peer map, mode
  ├── Processes signaling messages (posted from signaling IO thread)
  ├── Processes control commands (posted from control IO thread)
  ├── Forwards speak commands to ConversationEngine
  ├── Runs idle avatar timer (1fps, stopped during speech)
  └── DOES NOT do TTS or avatar rendering during speech

AudioLoop thread (dedicated std::thread, owned by ConversationEngine)
  ├── Reads audio-out shm directly (lock-free, no mutex)
  ├── Runs downsampler, AEC, VAD, STT feed
  ├── On SpeechEnd: runs Whisper transcription (< 200ms with Metal)
  ├── Spawns SpeechWorker threads
  └── Handles barge-in detection

SpeechWorker thread (std::thread, one at a time, owned by ConversationEngine)
  ├── Runs LLM streaming (libcurl, blocks on SSE) — autonomous only
  ├── Runs TTS per sentence (spawns Piper, reads PCM)
  ├── Resamples, writes audio-in shm, renders avatar, writes video-in shm
  ├── ALL Skia rendering happens here during speech (no cross-thread issue)
  └── Cancellable via atomic flag

WebRTC threads (owned by PeerConnectionFactory)
  ├── network_thread — ICE, DTLS
  ├── worker_thread — media processing
  └── signaling_thread — SDP, PeerConnection state

ADM capture thread (internal to ShmAudioDeviceModule)
  └── Reads audio-in shm every 10ms → WebRTC

ADM playout thread (internal to ShmAudioDeviceModule)
  └── Receives mixed peer audio from WebRTC → writes audio-out shm

Control IO thread (net::HttpServer)
  └── WebSocket command server

Signaling IO thread
  └── WebSocket client to meeting server
```

### Thread Safety Rules

1. **Audio-in shm**: written by SpeechWorker (TTS output), read by ADM capture thread. Safe: SPSC ring buffer (single writer, single reader).
2. **Audio-out shm**: written by ADM playout thread, read by AudioLoop. Safe: SPSC ring buffer.
3. **Video-in shm**: written by SpeechWorker (during speech) or main thread (idle frames). Safe: only one writes at a time — idle timer is stopped before SpeechWorker starts, restarted after it ends.
4. **Avatar renderer**: used by SpeechWorker during speech, by main thread for idle frames. Safe: idle timer stopped/started atomically around speech, never concurrent.
5. **ConversationEngine state** (`state_`, `running_`, `cancel_speech_`): `std::atomic` — lock-free.
6. **Conversation history**: only accessed by SpeechWorker thread — no concurrent access.

## Agent Modes

| Mode | Behavior | Use Case |
|------|----------|----------|
| `autonomous` | Listens via STT, thinks via LLM, speaks via TTS. Full conversation agent. | Real meetings |
| `scripted` | Only speaks when told via `speak` command. No STT, no LLM. | Testing, demos, orchestrated scenarios |

Default mode is `autonomous`. Switch via:
- `--mode=scripted` flag at launch
- `{"command": "set_mode", "mode": "autonomous"}` control command at runtime

In `scripted` mode, the AudioLoop still runs (for monitoring/metrics), but SpeechEnd events don't trigger the LLM pipeline.

### Scripted Speak Interaction with FSM

When a scripted `speak` command arrives while the agent is in autonomous mode:

| Current State | Behavior |
|--------------|----------|
| Listening | Immediately speak the scripted text (is_intro=true, skip LLM) |
| Thinking | Cancel LLM, speak the scripted text instead |
| Speaking (autonomous) | Queue the scripted text — spoken after current utterance finishes |
| Speaking (scripted) | Queue — FIFO order |

This means `Asmodeus.speak` always works regardless of mode. In autonomous mode it acts as an override/injection.

## Multi-Agent Turn-Taking

Full design in `Asmodeus/docs/TURN-TAKING-DESIGN.md`. Summary:

Based on Sacks, Schegloff & Jefferson (1974) turn-taking rules and the MMAgents framework (Frontiers in AI, 2025). Each agent autonomously decides whether to respond using a modular **TurnPolicy**:

### Architecture

```
AudioLoop → VAD → STT → TurnPolicy.Evaluate() → RESPOND or SILENT
```

### TurnPolicy Interface (pluggable)

| Policy | How | Speed | Use Case |
|--------|-----|-------|----------|
| `AlwaysRespondPolicy` | Always responds | Instant | Testing |
| `AdjacencyPairPolicy` | Rule-based: name detection, question detection | <1ms | Fast filter |
| `LlmTurnPolicy` | LLM decides + generates response in ONE call | ~1s | Production |

### The LLM Think+Respond Pattern

On hearing an utterance, the LLM receives the full **labeled conversation transcript** and decides:
- `RESPOND: [one-sentence response]` — agent speaks this (direct to TTS, no second LLM call)
- `SILENT: [reason]` — agent stays quiet

This is a SINGLE LLM call that combines "should I speak?" and "what should I say?". No random backoff. No coordinator. Each agent makes its own decision based on conversational context.

### Conversation Transcript

Each agent maintains a labeled transcript: `{speaker, text, timestamp}`.
- Own speech → labeled with own name
- 2-party: heard speech → labeled with the other participant's name
- 3+ party: labeled "other" unless coordinator broadcasts speaker identity
- Coordinator can broadcast `speaker_update` events for ground-truth labels

### Turn-Taking Rules (from conversation analysis)

1. **Current speaker selects next**: "Bob, what do you think?" → Bob must respond
2. **Self-selection**: open question → anyone may respond; first to start wins
3. **Current speaker continues**: if nobody responds, speaker may continue

## External Dependencies (runtime)

| Dependency | Location | Used For | Install |
|-----------|----------|----------|---------|
| Piper TTS | `~/.asmodeus/piper/piper` | Text-to-speech | `brew install piper` or download binary |
| Voice models | `~/.asmodeus/voices/*.onnx` | Piper voice data | Download from Piper releases |
| whisper.cpp | System lib (`libwhisper`) | Speech-to-text | `brew install whisper-cpp` |
| Whisper model | `~/.asmodeus/models/whisper/ggml-base.en.bin` | Whisper weights | Download from whisper.cpp |
| Silero VAD | `~/.asmodeus/models/silero_vad.onnx` | Voice activity detection | Download from Silero releases |
| ONNX Runtime | System lib (`libonnxruntime`) | Silero VAD inference | `brew install onnxruntime` |
| SpeexDSP | System lib (`libspeexdsp`) | Acoustic echo cancellation | `brew install speexdsp` |
| libcurl | System lib | LLM API calls | Already on macOS |

### Graceful Degradation

If autonomous-mode dependencies are missing at startup:
- Missing whisper model → fall back to scripted mode, log warning
- Missing silero model → fall back to scripted mode, log warning
- Missing ANTHROPIC_API_KEY → fall back to scripted mode, log warning
- Missing Piper → hard fail (required for both modes)
- Missing voice model → hard fail (required for both modes)

The agent never crashes due to missing optional dependencies. It always starts, possibly in degraded (scripted) mode.

## Command Line Flags

| Flag | Required | Default | Description |
|------|----------|---------|-------------|
| `--name` | Yes | — | Agent identifier (lowercase) |
| `--display-name` | No | Capitalized name | Display name |
| `--voice-model` | Yes | — | Path to Piper ONNX voice model |
| `--piper-path` | No | `~/.asmodeus/piper/piper` | Path to Piper binary |
| `--whisper-model` | No | `~/.asmodeus/models/whisper/ggml-base.en.bin` | Whisper model path |
| `--silero-model` | No | `~/.asmodeus/models/silero_vad.onnx` | Silero VAD model path |
| `--personality` | No | Generic assistant prompt | LLM system prompt |
| `--mode` | No | `autonomous` | `autonomous` or `scripted` |
| `--max-history` | No | 20 | Max conversation turns kept in LLM context |
| `--max-tokens` | No | 80 | Max LLM response tokens |
| `--response-delay-min` | No | 300 | Min random backoff before responding (ms) |
| `--response-delay-max` | No | 1500 | Max random backoff before responding (ms) |
| `--signaling` | Yes | — | WebSocket URL of signaling server |
| `--control-port` | Yes | — | Port for control channel |
| `--audio-shm` | No | `~/.asmodeus/audio-in-{name}.shm` | Audio output shm (our TTS) |
| `--audio-out-shm` | No | `~/.asmodeus/audio-out-{name}.shm` | Audio input shm (peer audio) |
| `--video-shm` | No | `~/.asmodeus/video-in-{name}.shm` | Video shm path |
| `--sample-rate` | No | 48000 | Audio sample rate |
| `--trace-file` | No | — | Chrome trace JSON output |

API key: read from `ANTHROPIC_API_KEY` environment variable. Never passed as a CLI flag (would be visible in `ps aux`).

## Control Channel Protocol

JSON messages over WebSocket.

### Commands (client → agent)

```json
{"id": 1, "command": "speak", "text": "Hello everyone."}
{"id": 2, "command": "get_state"}
{"id": 3, "command": "set_mode", "mode": "autonomous"}
{"id": 4, "command": "mute"}
{"id": 5, "command": "unmute"}
{"id": 6, "command": "set_voice", "model": "/path/to/voice.onnx"}
{"id": 7, "command": "shutdown"}
```

### Responses

```json
{"id": 1, "ok": true, "durationMs": 3200.5}
{"id": 2, "ok": true, "state": {
  "name": "alice",
  "mode": "autonomous",
  "fsmState": "listening",
  "speaking": false,
  "muted": false,
  "rms": 0.0,
  "peers": [
    {"peerId": "peer-1", "name": "Bob", "ice": "connected"}
  ],
  "speechQueueSize": 0,
  "turnsCompleted": 5,
  "historySize": 10,
  "uptimeMs": 45000
}}
```

### Events (agent → client, unsolicited)

```json
{"event": "heard", "text": "What do you think about that?", "sttMs": 180}
{"event": "thinking", "heardText": "What do you think about that?"}
{"event": "speech_started", "text": "I think it's a great idea.", "source": "llm"}
{"event": "speech_started", "text": "Hello everyone.", "source": "scripted"}
{"event": "speech_ended", "text": "I think it's a great idea.", "durationMs": 2100}
{"event": "barge_in"}
{"event": "peer_connected", "peerId": "peer-1", "name": "Bob"}
{"event": "peer_disconnected", "peerId": "peer-1"}
{"event": "mode_changed", "mode": "scripted", "reason": "missing whisper model"}
{"event": "error", "message": "LLM API returned 429"}
```

Note: `heard` has no `speaker` field — see Speaker Attribution section.

## Shared Memory Formats

### Audio SHM (AudioRingBuffer — existing)

```
Offset  Size    Type          Field
0       4       uint32_t      sample_rate (48000)
4       4       uint32_t      channels (1)
8       4       uint32_t      write_pos (atomic)
12      4       uint32_t      read_pos (atomic)
16      N*4     float32[]     ring buffer samples
```

Two shm files per agent:
- `audio-in-{name}.shm` — SpeechWorker writes TTS audio (coordinator + ADM capture read)
- `audio-out-{name}.shm` — ADM playout writes peer audio (AudioLoop reads)

Both are SPSC (single producer, single consumer). No locks needed.

### Video SHM (double-buffered NV12)

```
Offset  Size          Type          Field
0       4             uint32_t      width (640)
4       4             uint32_t      height (480)
8       4             uint32_t      frame_size
12      4             uint32_t      current_buffer (atomic, 0 or 1)
16      4             uint32_t      frame_sequence (atomic)
20      frame_size    uint8_t[]     buffer_0 (NV12)
20+fs   frame_size    uint8_t[]     buffer_1 (NV12)
```

Double-buffered: writer writes to the non-current buffer, then atomically flips `current_buffer`. Readers always read from the current buffer.

Writer: SpeechWorker during speech (at TTS chunk rate ~50fps), idle timer on main thread during silence (1fps). Never concurrent — idle timer is stopped/started around speech.

## WebRTC Audio Playout

The `ShmAudioDeviceModule` handles both capture and playout:

```cpp
class ShmAudioDeviceModule : public webrtc::AudioDeviceModule {
 public:
  ShmAudioDeviceModule(const std::string& capture_shm_path,
                       const std::string& playout_shm_path,
                       int sample_rate);

  // Capture: background thread reads audio-in shm every 10ms
  //   → calls AudioTransport::RecordedDataIsAvailable()
  //   → WebRTC encodes and sends to peers
  int32_t StartRecording() override;

  // Playout: background thread calls AudioTransport::NeedMorePlayData() every 10ms
  //   → receives mixed peer audio from WebRTC decoder
  //   → writes to audio-out shm ring buffer
  int32_t StartPlayout() override;
  bool Playing() const override { return playing_; }

 private:
  void CaptureLoop();   // reads audio-in shm → WebRTC
  void PlayoutLoop();   // WebRTC mixed peer audio → audio-out shm

  asmodeus::AudioRingBuffer capture_ring_;   // audio-in (our TTS)
  asmodeus::AudioRingBuffer playout_ring_;   // audio-out (peer audio)
};
```

AudioLoop reads from `audio-out` shm directly — same lock-free ring buffer. No `FeedAudio()` method, no mutex, no buffering layer. The playout thread writes, the AudioLoop thread reads. Classic SPSC.

## ConversationEngine — Core Component

Encapsulates the full listen→think→speak pipeline. Ported from `voice_agent/src/agent_fsm.cc`. Owns TTS, avatar rendering, and all audio I/O during speech.

```cpp
class ConversationEngine {
 public:
  struct Config {
    std::string whisper_model;
    std::string silero_model;
    std::string piper_path;
    std::string voice_model;
    std::string personality;
    int max_tokens = 80;
    int max_history = 20;       // sliding window of conversation turns
    float vad_threshold = 0.5f;
    int min_speech_ms = 100;
    int min_silence_ms = 200;
    int response_delay_min_ms = 300;   // random backoff range
    int response_delay_max_ms = 1500;
  };

  struct Callbacks {
    // All callbacks fire on AudioLoop or SpeechWorker thread.
    // Caller must PostTask to main thread if needed.
    std::function<void(const std::string& text, int stt_ms)> on_heard;
    std::function<void(const std::string& heard_text)> on_thinking;
    std::function<void(const std::string& text, const std::string& source)> on_speaking;
    std::function<void(const std::string& text, double duration_ms)> on_speech_ended;
    std::function<void()> on_barge_in;
    std::function<void(const std::string& error)> on_error;
  };

  ConversationEngine(Config config, Callbacks callbacks,
                     asmodeus::AudioRingBuffer* audio_in_shm,
                     asmodeus::AudioRingBuffer* audio_out_shm,
                     AsmodeusAvatarRenderer* avatar_renderer,
                     void* video_shm_mapped, size_t video_shm_size);
  ~ConversationEngine();

  // Returns true if all models loaded. Returns false and logs reason if not.
  // On false, caller should fall back to scripted mode.
  bool Init();

  void Start();  // Start AudioLoop thread
  void Stop();

  // Scripted speak — queued, processed by SpeechWorker
  void Speak(const std::string& text);

  void CancelSpeech();

  void SetAutonomous(bool autonomous);
  bool autonomous() const { return autonomous_.load(); }

  // Update LLM personality (called when peers join/leave to include their names)
  void SetPersonality(const std::string& personality);

  enum class State { Idle, Listening, Thinking, Speaking };
  State state() const { return state_.load(); }

 private:
  void AudioLoop();
  void SpeechWorker(std::string user_text, bool is_intro);

  // TTS + avatar output (called from SpeechWorker thread)
  void WriteTtsChunk(const float* samples_22k, int n);

  Config config_;
  Callbacks callbacks_;
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_speech_{false};
  std::atomic<bool> autonomous_{true};

  // Audio pipeline
  std::unique_ptr<SileroVAD> vad_;
  std::unique_ptr<WhisperEngine> stt_;
  std::unique_ptr<LlmStream> llm_;
  std::unique_ptr<Aec> aec_;
  std::unique_ptr<Downsampler48to16> downsampler_;

  // Shared resources (not owned, lifetime managed by NativeAgent)
  RAW_PTR_EXCLUSION asmodeus::AudioRingBuffer* audio_in_shm_;   // write TTS here
  RAW_PTR_EXCLUSION asmodeus::AudioRingBuffer* audio_out_shm_;  // read peer audio here
  RAW_PTR_EXCLUSION AsmodeusAvatarRenderer* avatar_renderer_;
  RAW_PTR_EXCLUSION void* video_shm_mapped_;
  size_t video_shm_size_;

  // Resampling scratch buffers (SpeechWorker thread only)
  std::vector<float> resample_48k_;
  std::vector<float> resample_16k_;

  std::thread audio_thread_;
  std::thread speech_worker_;

  // Conversation history — accessed only from SpeechWorker thread
  std::vector<LlmMessage> history_;

  // Speech queue — mutex-protected, written by main thread, read by AudioLoop
  std::mutex speak_mu_;
  std::queue<std::string> speak_queue_;
};
```

### Key Design Decisions

1. **Single TTS path**: Both autonomous (LLM→TTS) and scripted (speak command→TTS) go through the same `SpeechWorker`. No code duplication. `is_intro=true` skips the LLM call.

2. **No FeedAudio**: AudioLoop reads `audio-out` shm directly. Lock-free SPSC. No mutex between playout thread and AudioLoop.

3. **Avatar rendering on SpeechWorker thread**: During speech, `WriteTtsChunk()` calls `avatar_renderer_->Render()` directly. The idle timer on main thread is stopped before SpeechWorker runs. No concurrent Skia access.

4. **Shared resources not owned**: ConversationEngine receives pointers to shm buffers, avatar renderer, etc. NativeAgent owns their lifetime. This avoids duplicate resource management.

5. **History sliding window**: When `history_.size() > config_.max_history * 2`, drop the oldest half. Prevents unbounded growth during long meetings. The LLM always sees the most recent context.

## Integration with NativeAgent

```cpp
class NativeAgent {
  // Existing members...
  std::unique_ptr<asmodeus::AudioRingBuffer> audio_in_buffer_;   // our TTS output
  std::unique_ptr<asmodeus::AudioRingBuffer> audio_out_buffer_;  // peer audio input
  std::unique_ptr<AsmodeusAvatarRenderer> avatar_renderer_;

  // NEW
  std::unique_ptr<ConversationEngine> conversation_;
  base::RepeatingTimer idle_timer_;

  void InitConversation() {
    // Read API key from environment
    const char* api_key = getenv("ANTHROPIC_API_KEY");

    ConversationEngine::Config cfg;
    cfg.whisper_model = whisper_model_path_;
    cfg.silero_model = silero_model_path_;
    cfg.piper_path = piper_path_;
    cfg.voice_model = voice_model_path_;
    cfg.personality = BuildPersonalityPrompt();  // includes peer names
    cfg.max_history = max_history_;
    if (api_key) cfg.api_key = api_key;  // stored internally, not logged

    ConversationEngine::Callbacks cb;
    cb.on_heard = [this](const std::string& text, int stt_ms) {
      main_thread_->PostTask(FROM_HERE, base::BindOnce(
          &NativeAgent::OnHeard, base::Unretained(this), text, stt_ms));
    };
    cb.on_speaking = [this](const std::string& text, const std::string& source) {
      // Stop idle timer (SpeechWorker will render avatar)
      main_thread_->PostTask(FROM_HERE, base::BindOnce(
          &NativeAgent::StopIdleTimer, base::Unretained(this)));
      EmitEvent("speech_started", text, source);
    };
    cb.on_speech_ended = [this](const std::string& text, double ms) {
      // Restart idle timer
      main_thread_->PostTask(FROM_HERE, base::BindOnce(
          &NativeAgent::StartIdleTimer, base::Unretained(this)));
      EmitEvent("speech_ended", text, ms);
    };
    // ... other callbacks ...

    conversation_ = std::make_unique<ConversationEngine>(
        cfg, cb,
        audio_in_buffer_.get(),
        audio_out_buffer_.get(),
        avatar_renderer_.get(),
        video_shm_mapped_, video_shm_size_);

    if (!conversation_->Init()) {
      LOG(WARNING) << "ConversationEngine init failed, falling back to scripted mode";
      conversation_->SetAutonomous(false);
      EmitEvent("mode_changed", "scripted", "model load failed");
    } else if (mode_ == Mode::Scripted) {
      conversation_->SetAutonomous(false);
    }
    conversation_->Start();
  }

  // Existing Speak() now delegates entirely to ConversationEngine
  double Speak(const std::string& text) {
    conversation_->Speak(text);
    return 0;  // duration not known synchronously; speech_ended event has it
  }

  std::string BuildPersonalityPrompt() {
    // Include meeting context for multi-agent turn-taking.
    // Called at init and whenever peers change (join/leave).
    // ConversationEngine::SetPersonality() updates the LLM system prompt.
    std::string prompt = personality_;
    if (!peers_.empty()) {
      prompt += "\n\nYou are in a meeting with: ";
      for (auto& [id, info] : peers_) {
        prompt += info.display_name + ", ";
      }
      prompt += "\nOnly speak if addressed by name, asked a question, "
                "or you have something directly relevant to add. "
                "Keep responses to 1-2 sentences.";
    }
    return prompt;
  }

  // Called on peer join/leave to update LLM context
  void OnPeersChanged() {
    if (conversation_) {
      conversation_->SetPersonality(BuildPersonalityPrompt());
    }
  }
};
```

## File Layout

```
chromium/src/chrome/browser/asmodeus/native_agent/
  DESIGN.md                    # This document
  BUILD.gn                     # Build target
  main.cc                      # Entry point
  native_agent.h/cc            # Agent: owns state, peers, delegates to ConversationEngine
  # Layer 1: Audio Pipeline (signal processing)
  audio_pipeline.h/cc          # AudioLoop, VAD, AEC, STT, downsampler
  downsampler.h/cc             # 48kHz → 16kHz decimation
  # Layer 2: Dialogue Control (conversation logic)
  dialogue_manager.h/cc        # Transcript, speaker inference, policy dispatch
  turn_policy.h/cc             # TurnPolicy interface + AlwaysRespond + AdjacencyPair
  llm_turn_policy.h/cc         # LlmTurnPolicy (combined think+respond, single LLM call)
  # Layer 3: Speech Output (TTS + avatar)
  speech_output.h/cc           # Piper TTS, avatar rendering, shm output
  # Orchestrator
  conversation_engine.h/cc     # Thin wiring: Layer1 → Layer2 → Layer3
  avatar_renderer.h/cc         # Skia face renderer
  shm_audio_device_module.h/cc # AudioDeviceModule (capture + playout via shm)
  shm_video_source.h/cc        # VideoTrackSource from video shm
  signaling_client.h/cc        # Raw WebSocket signaling client
  control_channel.h/cc         # net::HttpServer WebSocket command server
  webrtc_peer.h/cc             # PeerConnection management
  video_shm.h                  # VideoShmHeader struct
```

## BUILD.gn Changes

```gn
executable("asmodeus_agent") {
  sources = [
    # ... existing sources ...
    "conversation_engine.cc",
    "conversation_engine.h",
    "downsampler.cc",
    "downsampler.h",
  ]

  deps = [
    # ... existing deps (base, net, webrtc, skia, libyuv, audio_ring_buffer) ...
  ]

  # External system libraries
  libs = [
    "whisper",       # whisper.cpp — STT
    "onnxruntime",   # Silero VAD inference
    "speexdsp",      # Acoustic echo cancellation
    "curl",          # LLM API (Anthropic)
  ]

  # Homebrew include paths (macOS)
  include_dirs = [
    "$homebrew_prefix/opt/whisper-cpp/include",
    "$homebrew_prefix/opt/onnxruntime/include/onnxruntime",
    "$homebrew_prefix/opt/speexdsp/include",
  ]

  lib_dirs = [
    "$homebrew_prefix/opt/whisper-cpp/lib",
    "$homebrew_prefix/opt/onnxruntime/lib",
    "$homebrew_prefix/opt/speexdsp/lib",
  ]
}
```

## Implementation Plan

### Phase 7: Playout + AudioLoop Shell

1. Modify `ShmAudioDeviceModule`: add playout support (write received peer audio to audio-out shm)
2. Create `audio-out-{name}.shm` in NativeAgent startup
3. Add `conversation_engine.h/cc` — shell with AudioLoop that reads audio-out shm, computes RMS, logs every 100 frames
4. Add `downsampler.h/cc` — 48k→16k decimation (port from voice_agent)
5. **Test**: Start 2 scripted agents. Agent A speaks via `speak` command. Verify agent B's audio-out shm has non-silent data. Verify AudioLoop logs RMS > 0 when peer is speaking, RMS ~0 when silent.

### Phase 8: AEC + VAD

1. Port `voice_agent/src/aec.cc` into ConversationEngine (SpeexDSP)
2. Port `voice_agent/src/silero_vad.cc` into ConversationEngine (ONNX Runtime)
3. Wire AudioLoop: downsample → AEC → VAD
4. AEC reference from TTS output (via `WriteTtsChunk` → `PushAecReference16k`)
5. Update BUILD.gn with libs: speexdsp, onnxruntime
6. **Test**: Agent A speaks. Agent B's AudioLoop detects SpeechStart and SpeechEnd via VAD. When B speaks (scripted) at the same time, AEC removes B's own voice — VAD only fires for A's audio.

### Phase 9: STT

1. Port `voice_agent/src/whisper_engine.cc` into ConversationEngine
2. Feed AEC-cleaned audio to Whisper continuously
3. On SpeechEnd: Transcribe(), emit `heard` event via callback
4. Update BUILD.gn with libs: whisper
5. **Test**: Agent A speaks "Hello, how are you doing today" (scripted). Agent B emits `heard` event with text containing "hello" and "how". Whisper latency < 500ms.

### Phase 10: LLM + Full Autonomous Loop

1. Port `voice_agent/src/llm_stream.cc` into ConversationEngine
2. On SpeechEnd in autonomous mode: random backoff → STT → LLM → streaming TTS
3. Wire TTS output path through ConversationEngine (unified with scripted speak)
4. Conversation history with sliding window
5. Update BUILD.gn with libs: curl
6. **Test**: Agent A (scripted) speaks "What is your favorite color?". Agent B (autonomous) hears, thinks, responds coherently via TTS. Verify: `heard`, `thinking`, `speech_started` events on B, audio in B's audio-in shm.

### Phase 11: Barge-in + Turn-Taking

1. VAD SpeechStart during Speaking/Thinking → cancel LLM + TTS
2. Random backoff before responding (configurable delay range)
3. Scripted speak interaction with FSM (see table above)
4. **Test**: Agent A speaks long text. While A is speaking, B's playout receives speech — B's VAD detects it, B cancels if speaking. Verify barge-in event, state transitions.

### Phase 12: Two-Agent Autonomous Demo

1. `createMeeting` → `addAgent("alice", autonomous)` → `addAgent("bob", autonomous)`
2. Kickstart: `Asmodeus.speak("alice", "Hi Bob, what do you think about AI agents?")`
3. Bob hears → thinks → responds → Alice hears → thinks → responds → ...
4. Monitor via control channel events for 10+ turns
5. `startRecording` → let them converse 2 minutes → `stopRecording`
6. **Verify**:
   - Alternating speech (not simultaneous for >1s)
   - Coherent dialogue (STT of recording makes sense)
   - Turn latency < 3s (SpeechEnd → first TTS chunk)
   - MP4 has lip-synced avatars for both agents

### Phase 13: Four-Agent Dynamic Meeting

1. Alice + Bob start autonomous conversation
2. Carol joins (personality: "designer"), introduced by Alice
3. Bob leaves, others adapt
4. Dave joins (personality: "infrastructure engineer"), introduced by Alice
5. Full recording with tile transitions
6. **Verify**:
   - No agent dominates (each speaks in at least 20% of turns they're present)
   - Agents mention newcomers/departures naturally
   - Correct tile count at each phase
   - Total duration > 3 minutes of real conversation
   - 20+ coherent turns total

## Relationship to Meeting Coordinator

The coordinator spawns agents and can set their mode:
- `addAgent` flag: `--mode=autonomous` or `--mode=scripted`
- Runtime: coordinator sends `set_mode` via control channel

`Asmodeus.speak` CDP command always works — in autonomous mode it injects a scripted utterance. In scripted mode it's the only way agents speak.

New CDP commands needed:
```
command setAgentMode
  parameters
    string name
    string mode   # "autonomous" or "scripted"

command addAgent
  parameters
    ...existing...
    optional string mode          # "autonomous" (default) or "scripted"
    optional string personality   # LLM system prompt
```

## Key Differences from voice_agent

| Aspect | voice_agent (standalone CMake) | native_agent (Chromium) |
|--------|-------------------------------|------------------------|
| Build | CMake + Homebrew | GN + Chromium deps + system libs |
| Audio input | Direct shm read (AudioFrameStream) | WebRTC playout → audio-out shm → AudioLoop |
| Audio output | Direct shm write (ShmAudio) | TTS → audio-in shm → WebRTC ADM capture |
| Video | None | Skia avatar → video-in shm → WebRTC video source |
| WebRTC | None | Full PeerConnection mesh |
| Control | stdin JSON | WebSocket server (net::HttpServer) |
| Tracing | Custom JSON tracer | base/trace_event (Perfetto) |
| Threading | std::thread only | base::Thread + std::thread |
| Multi-agent | N/A | Turn-taking via name-addressing + random backoff |
| Graceful degradation | Crash on missing model | Fall back to scripted mode |

## Build Commands

```bash
# Build agent (incremental)
cd chromium/src && third_party/ninja/ninja -C out/Default asmodeus_agent

# Build Chrome + agent
cd chromium/src && third_party/ninja/ninja -C out/Default chrome asmodeus_agent

# Run autonomous agent
ANTHROPIC_API_KEY=sk-... ./out/Default/asmodeus_agent \
  --name=alice \
  --mode=autonomous \
  --voice-model=$HOME/.asmodeus/voices/en_US-kristin-medium.onnx \
  --whisper-model=$HOME/.asmodeus/models/whisper/ggml-base.en.bin \
  --silero-model=$HOME/.asmodeus/models/silero_vad.onnx \
  --personality="You are Alice, a friendly meeting facilitator." \
  --signaling=ws://127.0.0.1:58000 \
  --control-port=9620

# Run scripted agent (no STT/LLM deps needed)
./out/Default/asmodeus_agent \
  --name=bob \
  --mode=scripted \
  --voice-model=$HOME/.asmodeus/voices/en_US-ryan-high.onnx \
  --signaling=ws://127.0.0.1:58000 \
  --control-port=9621

# NEVER touch args.gn or widely-included headers
# External libs linked via system paths, not Chromium third_party
```
