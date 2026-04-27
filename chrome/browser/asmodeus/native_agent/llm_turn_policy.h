// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_LLM_TURN_POLICY_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_LLM_TURN_POLICY_H_

#include <atomic>
#include <string>

#include "chrome/browser/asmodeus/native_agent/turn_policy.h"

namespace asmodeus {

// LLM-based turn policy: single API call that combines "should I respond?"
// and "what should I say?" into one RESPOND/SILENT output.
class LlmTurnPolicy : public TurnPolicy {
 public:
  struct Config {
    std::string api_key;
    std::string google_api_key;
    int max_tokens = 100;
  };

  explicit LlmTurnPolicy(Config config);

  DialogueAction Evaluate(
      const std::vector<TranscriptEntry>& transcript,
      const std::string& utterance,
      const std::string& my_name,
      const std::string& personality,
      const std::vector<std::string>& participants) override;

  void Cancel() override;

 private:
  std::string CallLlm(const std::string& prompt);
  static DialogueAction ParseResponse(const std::string& raw);

  Config config_;
  std::atomic<bool> cancel_{false};
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_LLM_TURN_POLICY_H_
