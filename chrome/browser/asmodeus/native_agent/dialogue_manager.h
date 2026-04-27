// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_DIALOGUE_MANAGER_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_DIALOGUE_MANAGER_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "chrome/browser/asmodeus/native_agent/turn_policy.h"

namespace asmodeus {

class LlmTurnPolicy;

class DialogueManager {
 public:
  struct Config {
    Config();
    ~Config();
    Config(const Config&);
    Config& operator=(const Config&);

    std::string my_name;
    std::string personality;
    std::string api_key;
    std::string google_api_key;
    int max_history = 20;
    int max_tokens = 100;
    std::string turn_policy = "llm";
  };

  explicit DialogueManager(Config config);
  ~DialogueManager();

  DialogueManager(const DialogueManager&) = delete;
  DialogueManager& operator=(const DialogueManager&) = delete;

  void SetParticipants(const std::vector<std::string>& names);

  // Record own speech (call BEFORE speaking). Thread-safe.
  void RecordOwnSpeech(const std::string& text, int64_t timestamp_ms);

  // Record heard speech without evaluating policy. Thread-safe.
  void RecordHeard(const std::string& text, int64_t timestamp_ms);

  // Core: heard an utterance, decide what to do.
  // Adds to transcript, runs policy chain. Thread-safe.
  // If should_respond=true, response_text is always non-empty.
  DialogueAction OnUtteranceHeard(const std::string& text, int64_t timestamp_ms);

  // Cancel in-flight LLM call.
  void CancelPending();

  // Coordinator-provided speaker identity.
  void OnSpeakerUpdate(const std::string& speaker, bool speaking);

  size_t transcript_size() const;

 private:
  std::string InferSpeaker() const;

  Config config_;
  std::vector<std::string> participants_;
  std::string current_speaker_;  // from coordinator broadcast

  mutable std::mutex mu_;
  std::vector<TranscriptEntry> transcript_;

  // Policy chain.
  std::unique_ptr<TurnPolicy> fast_policy_;  // AdjacencyPairPolicy
  std::unique_ptr<TurnPolicy> llm_policy_;   // LlmTurnPolicy (nullable)
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_DIALOGUE_MANAGER_H_
