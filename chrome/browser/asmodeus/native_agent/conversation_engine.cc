// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/conversation_engine.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include <speex/speex_echo.h>
#include <ggml-backend.h>
#include <whisper.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++98-compat-extra-semi"
#pragma clang diagnostic ignored "-Wextra-semi"
#include <onnxruntime_cxx_api.h>
#pragma clang diagnostic pop

#include "base/logging.h"
#include "chrome/browser/asmodeus/native_agent/downsampler.h"

namespace asmodeus {

// ── Audio pipeline constants ───────────────────────────────────

static constexpr int kAecFrameSize = 160;
static constexpr int kAecFilterLength = 3200;
static constexpr int kAecSampleRate = 16000;
static constexpr int kVadWindowSamples = 512;
static constexpr int kVadContextSamples = 64;
static constexpr int kVadInputSamples = kVadWindowSamples + kVadContextSamples;
static constexpr int kVadStateSize = 128;

namespace {
inline int16_t FloatToI16(float v) {
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return static_cast<int16_t>(std::lrintf(v * 32767.0f));
}
inline float I16ToFloat(int16_t v) {
  return static_cast<float>(v) / 32767.0f;
}
}  // namespace

struct ConversationEngine::VadImpl {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "silero_vad"};
  Ort::SessionOptions opts;
  std::unique_ptr<Ort::Session> session;
  Ort::MemoryInfo mem{
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)};
  std::array<float, 2 * 1 * kVadStateSize> state_in{};
  std::array<float, kVadContextSamples> context{};
  std::array<float, kVadInputSamples> input_buf{};
  int64_t sr_val = 16000;
  VadImpl() {
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
  }
};

// ── Config/Callbacks constructors ──────────────────────────────

ConversationEngine::Config::Config() = default;
ConversationEngine::Config::~Config() = default;
ConversationEngine::Config::Config(const Config&) = default;
ConversationEngine::Config& ConversationEngine::Config::operator=(const Config&) = default;
ConversationEngine::Config::Config(Config&&) = default;
ConversationEngine::Config& ConversationEngine::Config::operator=(Config&&) = default;

ConversationEngine::Callbacks::Callbacks() = default;
ConversationEngine::Callbacks::~Callbacks() = default;
ConversationEngine::Callbacks::Callbacks(const Callbacks&) = default;
ConversationEngine::Callbacks& ConversationEngine::Callbacks::operator=(const Callbacks&) = default;
ConversationEngine::Callbacks::Callbacks(Callbacks&&) = default;
ConversationEngine::Callbacks& ConversationEngine::Callbacks::operator=(Callbacks&&) = default;

// ── Constructor/Destructor ─────────────────────────────────────

ConversationEngine::ConversationEngine(Config config,
                                       Callbacks callbacks,
                                       AudioRingBuffer* audio_in_shm,
                                       AudioRingBuffer* audio_out_shm,
                                       AvatarRenderer* avatar_renderer,
                                       void* video_shm_mapped)
    : config_(std::move(config)),
      callbacks_(std::move(callbacks)),
      audio_out_shm_(audio_out_shm) {

  // Create Layer 2: DialogueManager.
  DialogueManager::Config dm_cfg;
  dm_cfg.my_name = config_.my_name;
  dm_cfg.personality = config_.personality;
  dm_cfg.api_key = config_.api_key;
  dm_cfg.google_api_key = config_.google_api_key;
  dm_cfg.max_history = config_.max_history;
  dm_cfg.max_tokens = config_.max_tokens;
  dm_cfg.turn_policy = config_.turn_policy;
  dialogue_ = std::make_unique<DialogueManager>(std::move(dm_cfg));

  // Create Layer 3: SpeechOutput.
  SpeechOutput::Config so_cfg;
  so_cfg.piper_path = config_.piper_path;
  so_cfg.voice_model = config_.voice_model;
  speech_ = std::make_unique<SpeechOutput>(
      std::move(so_cfg), audio_in_shm, avatar_renderer, video_shm_mapped,
      // AEC reference callback (Layer 3 → Layer 1).
      [this](const float* samples, int n) { AecPlayback(samples, n); },
      // OnSpeechStarted.
      [this](const std::string& text) {
        if (callbacks_.on_speaking) callbacks_.on_speaking(text, "tts");
      },
      // OnSpeechEnded.
      [this](const std::string& text, double ms) {
        if (callbacks_.on_speech_ended) callbacks_.on_speech_ended(text, ms);
      });
}

ConversationEngine::~ConversationEngine() {
  Stop();
  AecDestroy();
  SttDestroy();
}

// ── AEC (inline, Layer 1) ──────────────────────────────────────

void ConversationEngine::AecInit() {
  auto* st = speex_echo_state_init(kAecFrameSize, kAecFilterLength);
  int sr = kAecSampleRate;
  speex_echo_ctl(st, SPEEX_ECHO_SET_SAMPLING_RATE, &sr);
  aec_state_ = st;
  aec_rec_i16_.resize(kAecFrameSize);
  aec_out_i16_.resize(kAecFrameSize);
}

void ConversationEngine::AecDestroy() {
  if (aec_state_) {
    speex_echo_state_destroy(static_cast<SpeexEchoState*>(aec_state_));
    aec_state_ = nullptr;
  }
}

void ConversationEngine::AecCapture(const float* rec_16k, float* out_16k, int n) {
  if (!aec_state_ || n != kAecFrameSize) {
    if (out_16k && rec_16k) std::memcpy(out_16k, rec_16k, n * sizeof(float));
    return;
  }
  std::lock_guard<std::mutex> lock(aec_mu_);
  for (int i = 0; i < n; ++i) aec_rec_i16_[i] = FloatToI16(rec_16k[i]);
  speex_echo_capture(static_cast<SpeexEchoState*>(aec_state_),
                     aec_rec_i16_.data(), aec_out_i16_.data());
  for (int i = 0; i < n; ++i) out_16k[i] = I16ToFloat(aec_out_i16_[i]);
}

void ConversationEngine::AecPlayback(const float* ref_16k, int n) {
  if (!aec_state_) return;
  std::lock_guard<std::mutex> lock(aec_mu_);
  size_t start = aec_playback_buf_.size();
  aec_playback_buf_.resize(start + n);
  for (int i = 0; i < n; ++i) aec_playback_buf_[start + i] = FloatToI16(ref_16k[i]);
  auto* st = static_cast<SpeexEchoState*>(aec_state_);
  size_t pos = 0;
  while (aec_playback_buf_.size() - pos >= static_cast<size_t>(kAecFrameSize)) {
    speex_echo_playback(st, &aec_playback_buf_[pos]);
    pos += kAecFrameSize;
  }
  if (pos > 0) {
    aec_playback_buf_.erase(aec_playback_buf_.begin(),
                            aec_playback_buf_.begin() + static_cast<int>(pos));
  }
}

// ── VAD (inline, Layer 1) ──────────────────────────────────────

ConversationEngine::VadEvent
ConversationEngine::FeedVad(const float* samples_16k, int n) {
  if (!vad_impl_ || !vad_impl_->session || n != kVadWindowSamples)
    return VadEvent::None;
  auto& v = *vad_impl_;
  std::memcpy(v.input_buf.data(), v.context.data(), kVadContextSamples * sizeof(float));
  std::memcpy(v.input_buf.data() + kVadContextSamples, samples_16k, kVadWindowSamples * sizeof(float));
  std::memcpy(v.context.data(), samples_16k + kVadWindowSamples - kVadContextSamples, kVadContextSamples * sizeof(float));
  std::array<int64_t, 2> input_shape{1, kVadInputSamples};
  std::array<int64_t, 3> state_shape{2, 1, kVadStateSize};
  std::vector<int64_t> sr_shape;
  Ort::Value input_t = Ort::Value::CreateTensor<float>(v.mem, v.input_buf.data(), v.input_buf.size(), input_shape.data(), input_shape.size());
  Ort::Value state_t = Ort::Value::CreateTensor<float>(v.mem, v.state_in.data(), v.state_in.size(), state_shape.data(), state_shape.size());
  Ort::Value sr_t = Ort::Value::CreateTensor<int64_t>(v.mem, &v.sr_val, 1, sr_shape.data(), sr_shape.size());
  const char* in_names[] = {"input", "state", "sr"};
  const char* out_names[] = {"output", "stateN"};
  std::vector<Ort::Value> inputs;
  inputs.push_back(std::move(input_t));
  inputs.push_back(std::move(state_t));
  inputs.push_back(std::move(sr_t));
  std::vector<Ort::Value> outputs;
  try {
    outputs = v.session->Run(Ort::RunOptions{nullptr}, in_names, inputs.data(), inputs.size(), out_names, 2);
  } catch (const Ort::Exception&) { return VadEvent::None; }
  float prob = outputs[0].GetTensorMutableData<float>()[0];
  vad_last_prob_ = prob;
  std::memcpy(v.state_in.data(), outputs[1].GetTensorData<float>(), v.state_in.size() * sizeof(float));
  int min_sp = std::max(1, config_.min_speech_ms * kAecSampleRate / (kVadWindowSamples * 1000));
  int min_si = std::max(1, config_.min_silence_ms * kAecSampleRate / (kVadWindowSamples * 1000));
  bool is_speech = prob > config_.vad_threshold;
  if (is_speech) { vad_consec_speech_++; vad_consec_silence_ = 0; }
  else { vad_consec_silence_++; vad_consec_speech_ = 0; }
  if (!vad_in_speech_ && vad_consec_speech_ >= min_sp) { vad_in_speech_ = true; return VadEvent::SpeechStart; }
  if (vad_in_speech_ && vad_consec_silence_ >= min_si) { vad_in_speech_ = false; return VadEvent::SpeechEnd; }
  return VadEvent::None;
}

// ── STT (inline, Layer 1) ──────────────────────────────────────

namespace {
std::once_flag g_ggml_init;
void EnsureGgmlBackends() { std::call_once(g_ggml_init, []() { ggml_backend_load_all(); }); }
}  // namespace

bool ConversationEngine::SttInit() {
  if (config_.whisper_model.empty()) return false;
  EnsureGgmlBackends();
  struct whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = true;
  whisper_ctx_ = whisper_init_from_file_with_params(config_.whisper_model.c_str(), cparams);
  return whisper_ctx_ != nullptr;
}
void ConversationEngine::SttDestroy() {
  if (whisper_ctx_) { whisper_free(static_cast<whisper_context*>(whisper_ctx_)); whisper_ctx_ = nullptr; }
}
void ConversationEngine::SttFeed(const float* s, int n) { stt_buffer_.insert(stt_buffer_.end(), s, s + n); }
std::string ConversationEngine::SttTranscribe() {
  if (!whisper_ctx_ || stt_buffer_.empty()) return "";
  auto* ctx = static_cast<whisper_context*>(whisper_ctx_);
  whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.print_progress = false; params.print_special = false;
  params.print_realtime = false; params.print_timestamps = false;
  params.language = "en"; params.n_threads = 4;
  params.translate = false; params.no_context = true;
  params.suppress_blank = true;
  if (whisper_full(ctx, params, stt_buffer_.data(), static_cast<int>(stt_buffer_.size())) != 0) return "";
  std::string out;
  for (int i = 0; i < whisper_full_n_segments(ctx); ++i) {
    const char* t = whisper_full_get_segment_text(ctx, i);
    if (t) { if (!out.empty()) out += " "; out += t; }
  }
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
  return out;
}
void ConversationEngine::SttReset() { stt_buffer_.clear(); }

// ── Init ───────────────────────────────────────────────────────

bool ConversationEngine::Init() {
  AecInit();
  LOG(INFO) << "ConversationEngine: AEC initialized";
  if (!config_.silero_model.empty()) {
    vad_impl_ = std::make_unique<VadImpl>();
    try {
      vad_impl_->session = std::make_unique<Ort::Session>(
          vad_impl_->env, config_.silero_model.c_str(), vad_impl_->opts);
      LOG(INFO) << "ConversationEngine: VAD loaded";
    } catch (const Ort::Exception& e) {
      LOG(WARNING) << "VAD load failed: " << e.what();
      vad_impl_.reset();
    }
  }
  if (SttInit()) LOG(INFO) << "ConversationEngine: Whisper loaded";
  else if (!config_.whisper_model.empty()) LOG(WARNING) << "Whisper load failed";
  return true;
}

// ── Start/Stop ─────────────────────────────────────────────────

void ConversationEngine::Start() {
  if (running_.exchange(true)) return;
  state_.store(State::Listening);
  audio_thread_ = std::thread(&ConversationEngine::AudioLoop, this);
  LOG(INFO) << "ConversationEngine started";
}

void ConversationEngine::Stop() {
  if (!running_.exchange(false)) return;
  cancel_.store(true);
  if (audio_thread_.joinable()) audio_thread_.join();
  if (speech_worker_.joinable()) speech_worker_.join();
  state_.store(State::Idle);
  LOG(INFO) << "ConversationEngine stopped";
}

void ConversationEngine::Speak(const std::string& text) {
  std::lock_guard<std::mutex> lock(speak_mu_);
  speak_queue_.push(text);
}

void ConversationEngine::CancelSpeech() {
  cancel_.store(true);
  if (dialogue_) dialogue_->CancelPending();
  if (speech_) speech_->Cancel();
}

void ConversationEngine::SetParticipants(const std::vector<std::string>& names) {
  if (dialogue_) dialogue_->SetParticipants(names);
}

void ConversationEngine::SetPersonality(const std::string& p) {
  config_.personality = p;
}

void ConversationEngine::OnSpeakerUpdate(const std::string& speaker, bool speaking) {
  if (dialogue_) dialogue_->OnSpeakerUpdate(speaker, speaking);
}

size_t ConversationEngine::transcript_size() const {
  return dialogue_ ? dialogue_->transcript_size() : 0;
}

// ── Orchestrator: OnUtterance ──────────────────────────────────

void ConversationEngine::OnUtterance(const std::string& text, int stt_ms,
                                     int64_t timestamp_ms) {
  if (callbacks_.on_heard) callbacks_.on_heard(text, stt_ms);

  // If busy (thinking/speaking), just record in transcript.
  if (state_.load() != State::Listening) {
    if (dialogue_) dialogue_->RecordHeard(text, timestamp_ms);
    return;
  }

  // Spawn SpeechWorker (non-blocking — AudioLoop continues for barge-in).
  if (speech_worker_.joinable()) speech_worker_.join();
  speech_worker_ = std::thread(
      &ConversationEngine::SpeechWorkerFn, this, text, false);
}

void ConversationEngine::OnBargeIn() {
  LOG(INFO) << "Barge-in detected";
  cancel_.store(true);
  if (dialogue_) dialogue_->CancelPending();
  if (speech_) speech_->Cancel();
  if (callbacks_.on_barge_in) callbacks_.on_barge_in();
  state_.store(State::Listening);
}

// ── SpeechWorker ───────────────────────────────────────────────

void ConversationEngine::SpeechWorkerFn(std::string text, bool is_scripted) {
  cancel_.store(false);

  if (is_scripted) {
    // Scripted speak — direct to TTS, no dialogue evaluation.
    state_.store(State::Speaking);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    if (dialogue_) dialogue_->RecordOwnSpeech(text, ms);
    if (speech_) speech_->Speak(text);
    state_.store(State::Listening);
    return;
  }

  // Autonomous: evaluate dialogue → maybe respond.
  state_.store(State::Thinking);
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

  DialogueAction action;
  if (dialogue_) {
    action = dialogue_->OnUtteranceHeard(text, ms);
  } else {
    action = {true, text, "no dialogue manager"};
  }

  if (cancel_.load()) { state_.store(State::Listening); return; }

  if (!action.should_respond) {
    LOG(INFO) << "[" << config_.my_name << "] SILENT: " << action.reason;
    state_.store(State::Listening);
    return;
  }

  // Random jitter before responding. Uses name hash for a per-agent
  // base offset (0-2s) plus random (0-1s). This gives each agent a
  // distinct response window, preventing simultaneous identical responses.
  // The agent that starts first triggers the other's listen-before-speak.
  {
    uint32_t h = 0;
    for (char c : config_.my_name) h = h * 31 + c;
    int base_ms = (h % 2000);  // 0-2000ms per-agent offset
    int rand_ms = rand() % 1000;  // 0-1000ms random
    int jitter_ms = base_ms + rand_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter_ms));
  }

  // Listen-before-speak: did someone else start talking while we were thinking?
  if (vad_in_speech_) {
    LOG(INFO) << "[" << config_.my_name << "] Yielding — someone else speaking";
    state_.store(State::Listening);
    return;
  }

  // Speak the response.
  std::string response = action.response_text;
  if (response.empty()) {
    // Policy didn't generate text (shouldn't happen with LlmTurnPolicy).
    LOG(WARNING) << "RESPOND with empty text, staying silent";
    state_.store(State::Listening);
    return;
  }

  LOG(INFO) << "[" << config_.my_name << "] RESPOND: " << response;
  if (dialogue_) {
    auto now2 = std::chrono::steady_clock::now().time_since_epoch();
    auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(now2).count();
    dialogue_->RecordOwnSpeech(response, ms2);
  }
  state_.store(State::Speaking);
  if (speech_) speech_->Speak(response);
  state_.store(State::Listening);
}

// ── AudioLoop ──────────────────────────────────────────────────

void ConversationEngine::AudioLoop() {
  const int frame_samples = 480;
  float frame48[480];
  Downsampler48to16 downsampler;
  std::vector<float> frame16;
  frame16.reserve(160);
  float aec_out[kAecFrameSize];
  std::vector<float> vad_buf;
  vad_buf.reserve(kVadWindowSamples * 8);
  uint64_t frames_read = 0;

  while (running_.load()) {
    // Check scripted speak queue.
    {
      std::lock_guard<std::mutex> lock(speak_mu_);
      if (!speak_queue_.empty() && state_.load() == State::Listening) {
        std::string text = speak_queue_.front();
        speak_queue_.pop();
        if (speech_worker_.joinable()) speech_worker_.join();
        speech_worker_ = std::thread(
            &ConversationEngine::SpeechWorkerFn, this, std::move(text), true);
      }
    }

    if (!audio_out_shm_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    size_t read = audio_out_shm_->Read(frame48, frame_samples);
    if (read == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    ++frames_read;

    frame16.clear();
    downsampler.Process(frame48, static_cast<int>(read), frame16);

    float rms = 0.0f;
    for (size_t i = 0; i < read; ++i) rms += frame48[i] * frame48[i];
    rms = std::sqrt(rms / static_cast<float>(read));
    input_rms_.store(rms);

    if (static_cast<int>(frame16.size()) == kAecFrameSize) {
      AecCapture(frame16.data(), aec_out, kAecFrameSize);
    } else {
      std::memcpy(aec_out, frame16.data(),
                  std::min(frame16.size(), static_cast<size_t>(kAecFrameSize)) * sizeof(float));
    }

    SttFeed(aec_out, kAecFrameSize);

    vad_buf.insert(vad_buf.end(), aec_out, aec_out + kAecFrameSize);
    while (vad_buf.size() >= kVadWindowSamples) {
      VadEvent ev = FeedVad(vad_buf.data(), kVadWindowSamples);
      vad_buf.erase(vad_buf.begin(), vad_buf.begin() + kVadWindowSamples);

      if (ev == VadEvent::SpeechStart || ev == VadEvent::SpeechEnd) {
        last_voice_frame_ = frames_read;
      }
      if (ev == VadEvent::SpeechStart) {
        if (state_.load() == State::Speaking || state_.load() == State::Thinking) {
          OnBargeIn();
        }
        SttReset();
      } else if (ev == VadEvent::SpeechEnd) {
        auto t0 = std::chrono::steady_clock::now();
        std::string text = SttTranscribe();
        auto stt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        SttReset();
        if (text.size() >= 3) {
          LOG(INFO) << "STT: \"" << text << "\" (" << stt_ms << "ms)";
          auto now = std::chrono::steady_clock::now().time_since_epoch();
          auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
          OnUtterance(text, static_cast<int>(stt_ms), ts);
        }
      }
    }

    // Silence-triggered topic initiation.
    // Each agent has a different timeout based on name hash to prevent
    // all agents from initiating simultaneously. Range: 8-20 seconds.
    // Agents who spoke recently get longer timeouts (fairness).
    if (silence_threshold_ == 0) {
      // Compute once: hash name to get a per-agent offset
      uint32_t h = 0;
      for (char c : config_.my_name) h = h * 31 + c;
      silence_threshold_ = 800 + (h % 1200);  // 800-2000 frames (8-20s)
    }
    if (frames_read > 0 && last_voice_frame_ > 0 &&
        (frames_read - last_voice_frame_) > silence_threshold_ &&
        state_.load() == State::Listening &&
        !vad_in_speech_ &&
        dialogue_ && dialogue_->transcript_size() > 0) {
      last_voice_frame_ = frames_read;  // Reset so we don't trigger again
      // Increase threshold after each initiation (back off)
      silence_threshold_ += 500;  // +5 seconds per initiation
      LOG(INFO) << "[" << config_.my_name
                << "] Silence timeout — initiating topic (next threshold="
                << silence_threshold_ << ")";
      if (speech_worker_.joinable()) speech_worker_.join();
      speech_worker_ = std::thread(
          &ConversationEngine::SpeechWorkerFn, this,
          "[SILENCE_TIMEOUT: It's been quiet. Bring up a new topic or ask a follow-up question.]",
          false);
    }

    if (frames_read % 100 == 0) {
      LOG(INFO) << "AudioLoop: frame=" << frames_read << " rms=" << rms
                << " state=" << static_cast<int>(state_.load());
    }
  }
}

}  // namespace asmodeus
