// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/dialogue_manager.h"

#include <algorithm>
#include <cctype>

#include "base/logging.h"
#include "chrome/browser/asmodeus/native_agent/llm_turn_policy.h"

namespace asmodeus {

namespace {
bool ContainsIgnoreCase(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return false;
  auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
  return it != haystack.end();
}
}  // namespace

DialogueManager::Config::Config() = default;
DialogueManager::Config::~Config() = default;
DialogueManager::Config::Config(const Config&) = default;
DialogueManager::Config& DialogueManager::Config::operator=(const Config&) = default;

DialogueManager::DialogueManager(Config config)
    : config_(std::move(config)) {
  // Create policy chain based on config.
  if (config_.turn_policy == "always") {
    fast_policy_ = std::make_unique<AlwaysRespondPolicy>();
  } else {
    // Default: AdjacencyPair fast filter.
    fast_policy_ = std::make_unique<AdjacencyPairPolicy>();
  }

  // LLM policy (if we have an API key and policy != "always" && != "adjacency").
  if (config_.turn_policy == "llm" &&
      (!config_.api_key.empty() || !config_.google_api_key.empty())) {
    LlmTurnPolicy::Config llm_cfg;
    llm_cfg.api_key = config_.api_key;
    llm_cfg.google_api_key = config_.google_api_key;
    llm_cfg.max_tokens = config_.max_tokens;
    llm_policy_ = std::make_unique<LlmTurnPolicy>(std::move(llm_cfg));
    LOG(INFO) << "DialogueManager: LLM turn policy enabled for " << config_.my_name;
  }
}

DialogueManager::~DialogueManager() = default;

void DialogueManager::SetParticipants(const std::vector<std::string>& names) {
  std::lock_guard<std::mutex> lock(mu_);
  participants_ = names;
}

std::string DialogueManager::InferSpeaker() const {
  // Must be called with mu_ held.
  if (!current_speaker_.empty()) return current_speaker_;
  if (participants_.size() == 2) {
    for (const auto& p : participants_) {
      if (!ContainsIgnoreCase(p, config_.my_name)) return p;
    }
  }
  return "Someone";
}

void DialogueManager::RecordOwnSpeech(const std::string& text,
                                       int64_t timestamp_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  transcript_.push_back({config_.my_name, text, timestamp_ms});
  // Trim transcript.
  size_t max_entries = static_cast<size_t>(config_.max_history * 2);
  if (transcript_.size() > max_entries) {
    transcript_.erase(transcript_.begin(),
                      transcript_.begin() +
                          static_cast<int>(transcript_.size() - max_entries));
  }
}

void DialogueManager::RecordHeard(const std::string& text,
                                   int64_t timestamp_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  transcript_.push_back({InferSpeaker(), text, timestamp_ms});
  size_t max_entries = static_cast<size_t>(config_.max_history * 2);
  if (transcript_.size() > max_entries) {
    transcript_.erase(transcript_.begin(),
                      transcript_.begin() +
                          static_cast<int>(transcript_.size() - max_entries));
  }
}

DialogueAction DialogueManager::OnUtteranceHeard(const std::string& text,
                                                  int64_t timestamp_ms) {
  std::vector<TranscriptEntry> transcript_copy;
  std::vector<std::string> participants_copy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    // Add to transcript.
    transcript_.push_back({InferSpeaker(), text, timestamp_ms});
    size_t max_entries = static_cast<size_t>(config_.max_history * 2);
    if (transcript_.size() > max_entries) {
      transcript_.erase(transcript_.begin(),
                        transcript_.begin() +
                            static_cast<int>(transcript_.size() - max_entries));
    }
    transcript_copy = transcript_;
    participants_copy = participants_;
  }

  // Silence timeout — generate a new topic directly via LLM.
  if (text.find("[SILENCE_TIMEOUT:") == 0 && llm_policy_) {
    LOG(INFO) << "[" << config_.my_name << "] Silence timeout — asking LLM for new topic";
    DialogueAction action = llm_policy_->Evaluate(
        transcript_copy, text, config_.my_name,
        config_.personality +
            "\n\nThe meeting has gone silent. You should bring up a new topic, "
            "ask a follow-up question, or summarize what was discussed. "
            "You MUST respond with something to keep the meeting going. "
            "ONE sentence only.",
        participants_copy);
    if (action.should_respond && !action.response_text.empty()) {
      LOG(INFO) << "[" << config_.my_name << "] RESPOND (new topic): "
                << action.response_text;
      return action;
    }
    return {false, "", "LLM declined to initiate"};
  }

  // Stage 1: Fast policy (AdjacencyPairPolicy).
  DialogueAction fast_action = fast_policy_->Evaluate(
      transcript_copy, text, config_.my_name,
      config_.personality, participants_copy);

  if (!fast_action.should_respond) {
    // Fast-reject: definitely not addressed.
    LOG(INFO) << "[" << config_.my_name << "] SILENT (fast): "
              << fast_action.reason;
    return fast_action;
  }

  // Adjacency pair completion: I asked, they answered.
  // Only trigger if I haven't spoken in the last 2 transcript entries
  // (prevents infinite follow-up loops where my follow-up triggers
  // another response which triggers another follow-up).
  if (fast_action.reason == "my question was answered" && llm_policy_) {
    // Check if I spoke very recently (last 2 entries) — if so, skip follow-up
    bool spoke_very_recently = false;
    if (transcript_copy.size() >= 3) {
      size_t check_start = transcript_copy.size() - 3;
      for (size_t i = check_start; i < transcript_copy.size() - 1; ++i) {
        if (ContainsIgnoreCase(transcript_copy[i].speaker, config_.my_name)) {
          spoke_very_recently = true;
          break;
        }
      }
    }
    if (spoke_very_recently) {
      LOG(INFO) << "[" << config_.my_name
                << "] SILENT: adjacency pair but spoke very recently (avoiding loop)";
      return {false, "", "spoke very recently, avoiding follow-up loop"};
    }
    LOG(INFO) << "[" << config_.my_name
              << "] Adjacency pair complete — generating follow-up";
    DialogueAction follow_up = llm_policy_->Evaluate(
        transcript_copy, text, config_.my_name,
        config_.personality +
            "\n\nThe previous speaker just answered YOUR question. "
            "Briefly acknowledge and move on. ONE short sentence only.",
        participants_copy);
    if (!follow_up.should_respond || follow_up.response_text.empty()) {
      follow_up.should_respond = true;
      follow_up.response_text = "Thank you for that update.";
      follow_up.reason = "adjacency pair forced";
    }
    LOG(INFO) << "[" << config_.my_name << "] RESPOND (follow-up): "
              << follow_up.response_text;
    return follow_up;
  }

  // Stage 2: LLM policy (if available).
  if (llm_policy_) {
    DialogueAction llm_action = llm_policy_->Evaluate(
        transcript_copy, text, config_.my_name,
        config_.personality, participants_copy);
    LOG(INFO) << "[" << config_.my_name << "] "
              << (llm_action.should_respond ? "RESPOND" : "SILENT")
              << " (LLM): " << llm_action.reason;
    return llm_action;
  }

  // No LLM policy — fast policy said respond but has no text.
  LOG(INFO) << "[" << config_.my_name << "] RESPOND (fast only): "
            << fast_action.reason;
  return fast_action;
}

void DialogueManager::CancelPending() {
  if (llm_policy_) llm_policy_->Cancel();
}

void DialogueManager::OnSpeakerUpdate(const std::string& speaker,
                                       bool speaking) {
  std::lock_guard<std::mutex> lock(mu_);
  current_speaker_ = speaking ? speaker : "";
}

size_t DialogueManager::transcript_size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return transcript_.size();
}

}  // namespace asmodeus
