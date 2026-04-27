// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/turn_policy.h"

#include <algorithm>
#include <cctype>

namespace asmodeus {

namespace {
// Case-insensitive substring search.
bool ContainsIgnoreCase(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return false;
  auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
  return it != haystack.end();
}

bool EndsWithQuestion(const std::string& text) {
  for (int i = static_cast<int>(text.size()) - 1; i >= 0; --i) {
    if (text[i] == '?') return true;
    if (!std::isspace(text[i])) return false;
  }
  return false;
}
}  // namespace

// ── AlwaysRespondPolicy ──────────────────────────────────────

DialogueAction AlwaysRespondPolicy::Evaluate(
    const std::vector<TranscriptEntry>&,
    const std::string&,
    const std::string&,
    const std::string&,
    const std::vector<std::string>&) {
  return {true, "", "always respond"};
}

// ── AdjacencyPairPolicy ─────────────────────────────────────

DialogueAction AdjacencyPairPolicy::Evaluate(
    const std::vector<TranscriptEntry>& transcript,
    const std::string& utterance,
    const std::string& my_name,
    const std::string&,
    const std::vector<std::string>& participants) {

  // Rule 0: Adjacency pair completion — I asked a question, someone answered.
  // I should continue immediately (acknowledge, follow up, or move on).
  if (transcript.size() >= 2) {
    // Find my last utterance in the transcript (before the current one).
    for (int i = static_cast<int>(transcript.size()) - 2; i >= 0; --i) {
      if (ContainsIgnoreCase(transcript[i].speaker, my_name)) {
        if (EndsWithQuestion(transcript[i].text)) {
          return {true, "", "my question was answered"};
        }
        break;  // Found my last utterance, it wasn't a question.
      }
    }
  }

  // Rule 1: Am I addressed by name?
  if (ContainsIgnoreCase(utterance, my_name)) {
    return {true, "", "addressed by name"};
  }

  // Rule 2: Is another participant addressed by name? → SILENT
  for (const auto& p : participants) {
    if (p != my_name && ContainsIgnoreCase(utterance, p)) {
      return {false, "", p + " was addressed"};
    }
  }

  // Rule 3: Open question (no name, ends with ?)
  if (EndsWithQuestion(utterance)) {
    // Return RESPOND with empty text — caller must generate via LLM.
    return {true, "", "open question"};
  }

  // Rule 4: Statement not addressed to anyone. Check recency — if I haven't
  // spoken in the last few turns, I may self-select.
  int my_recent = 0;
  size_t start = transcript.size() > 6 ? transcript.size() - 6 : 0;
  for (size_t i = start; i < transcript.size(); ++i) {
    if (ContainsIgnoreCase(transcript[i].speaker, my_name)) {
      my_recent++;
    }
  }
  if (my_recent == 0) {
    return {true, "", "self-selection: haven't spoken recently"};
  }

  return {false, "", "not addressed, spoke recently"};
}

}  // namespace asmodeus
