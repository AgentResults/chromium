// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/llm_turn_policy.h"

#include <cstring>
#include <sstream>

#include <curl/curl.h>

#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"

namespace asmodeus {

namespace {
std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

struct CurlWriteCtx {
  std::string response;
  RAW_PTR_EXCLUSION std::atomic<bool>* cancel;
};

size_t OnCurlData(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<CurlWriteCtx*>(userdata);
  if (ctx->cancel->load()) return 0;
  ctx->response.append(ptr, size * nmemb);
  return size * nmemb;
}
}  // namespace

LlmTurnPolicy::LlmTurnPolicy(Config config)
    : config_(std::move(config)) {}

void LlmTurnPolicy::Cancel() {
  cancel_.store(true);
}

DialogueAction LlmTurnPolicy::Evaluate(
    const std::vector<TranscriptEntry>& transcript,
    const std::string& utterance,
    const std::string& my_name,
    const std::string& personality,
    const std::vector<std::string>& participants) {
  cancel_.store(false);

  // Build the prompt.
  std::ostringstream prompt;
  prompt << personality << "\n\n";
  prompt << "You are in a meeting with: ";
  for (size_t i = 0; i < participants.size(); ++i) {
    if (i > 0) prompt << ", ";
    prompt << participants[i];
  }
  prompt << ".\n\n";

  // Transcript (last N entries).
  size_t start = transcript.size() > 20 ? transcript.size() - 20 : 0;
  if (start < transcript.size()) {
    prompt << "Conversation so far:\n";
    for (size_t i = start; i < transcript.size(); ++i) {
      prompt << transcript[i].speaker << ": " << transcript[i].text << "\n";
    }
    prompt << "\n";
  }

  prompt << "Someone just said: \"" << utterance << "\"\n\n";
  prompt << "Should you respond? Reply with exactly one of:\n";
  prompt << "RESPOND: [your one-sentence reply]\n";
  prompt << "SILENT: [brief reason]\n";

  std::string raw = CallLlm(prompt.str());
  if (cancel_.load() || raw.empty()) {
    return {false, "", "cancelled or empty"};
  }
  return ParseResponse(raw);
}

std::string LlmTurnPolicy::CallLlm(const std::string& prompt) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";

  bool use_gemini = config_.api_key.empty() && !config_.google_api_key.empty();

  std::string body_str;
  std::string url;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  if (use_gemini) {
    url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=" + config_.google_api_key;
    body_str = R"({"contents":[{"parts":[{"text":")" +
        JsonEscape(prompt) +
        R"("}]}],"generationConfig":{"maxOutputTokens":)" +
        std::to_string(config_.max_tokens) + "}}";
  } else {
    url = "https://api.anthropic.com/v1/messages";
    std::string auth = "x-api-key: " + config_.api_key;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    body_str = R"({"model":"claude-sonnet-4-20250514","max_tokens":)" +
        std::to_string(config_.max_tokens) +
        R"(,"messages":[{"role":"user","content":")" +
        JsonEscape(prompt) + R"("}]})";
  }

  CurlWriteCtx ctx;
  ctx.cancel = &cancel_;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnCurlData);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

  CURLcode rc = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK || cancel_.load()) return "";

  // Extract text from JSON response.
  // Anthropic: {"content":[{"text":"..."}]}
  // Gemini: {"candidates":[{"content":{"parts":[{"text":"..."}]}}]}
  std::string& raw = ctx.response;
  auto pos = raw.find("\"text\":\"");
  if (pos == std::string::npos) {
    // Try Anthropic format
    pos = raw.find("\"text\":\"");
  }
  if (pos == std::string::npos) {
    LOG(WARNING) << "LlmTurnPolicy: no text in response: "
                 << raw.substr(0, 200);
    return "";
  }
  pos += 8;
  std::string text;
  for (size_t i = pos; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '\\' && i + 1 < raw.size()) {
      char next = raw[i + 1];
      if (next == 'n') text.push_back('\n');
      else if (next == '"') text.push_back('"');
      else if (next == '\\') text.push_back('\\');
      else text.push_back(next);
      ++i;
    } else if (c == '"') {
      break;
    } else {
      text.push_back(c);
    }
  }
  return text;
}

DialogueAction LlmTurnPolicy::ParseResponse(const std::string& raw) {
  // Check for RESPOND: prefix
  if (raw.size() > 9 && raw.substr(0, 9) == "RESPOND: ") {
    return {true, raw.substr(9), "LLM decided to respond"};
  }
  if (raw.size() > 8 && raw.substr(0, 8) == "RESPOND:") {
    return {true, raw.substr(8), "LLM decided to respond"};
  }
  // Check for SILENT: prefix
  if (raw.size() > 8 && raw.substr(0, 8) == "SILENT: ") {
    return {false, "", raw.substr(8)};
  }
  if (raw.size() > 7 && raw.substr(0, 7) == "SILENT:") {
    return {false, "", raw.substr(7)};
  }
  // Graceful fallback: treat as response
  if (!raw.empty()) {
    return {true, raw, "fallback: no prefix"};
  }
  return {false, "", "empty response"};
}

}  // namespace asmodeus
