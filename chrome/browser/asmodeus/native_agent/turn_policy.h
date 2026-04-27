// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_TURN_POLICY_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_TURN_POLICY_H_

#include <string>
#include <vector>

namespace asmodeus {

struct TranscriptEntry {
  std::string speaker;
  std::string text;
  int64_t timestamp_ms = 0;
};

struct DialogueAction {
  bool should_respond = false;
  std::string response_text;
  std::string reason;
};

class TurnPolicy {
 public:
  virtual ~TurnPolicy() = default;

  virtual DialogueAction Evaluate(
      const std::vector<TranscriptEntry>& transcript,
      const std::string& utterance,
      const std::string& my_name,
      const std::string& personality,
      const std::vector<std::string>& participants) = 0;

  // Cancel any in-flight work (e.g. LLM call). Thread-safe.
  virtual void Cancel() {}
};

// Always responds. For testing only.
class AlwaysRespondPolicy : public TurnPolicy {
 public:
  DialogueAction Evaluate(
      const std::vector<TranscriptEntry>& transcript,
      const std::string& utterance,
      const std::string& my_name,
      const std::string& personality,
      const std::vector<std::string>& participants) override;
};

// Rule-based: detects name-addressing and adjacency pairs.
// Fast (<1ms), no LLM call. Used as first stage in the policy chain.
class AdjacencyPairPolicy : public TurnPolicy {
 public:
  // Returns RESPOND if addressed by name, SILENT if another name addressed,
  // or {should_respond=true, response_text=""} for open questions
  // (caller must generate response separately).
  DialogueAction Evaluate(
      const std::vector<TranscriptEntry>& transcript,
      const std::string& utterance,
      const std::string& my_name,
      const std::string& personality,
      const std::vector<std::string>& participants) override;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_TURN_POLICY_H_
