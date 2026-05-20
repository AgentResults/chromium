// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_CREDENTIAL_STORE_H_
#define CHROME_BROWSER_ASMODEUS_CREDENTIAL_STORE_H_

#include <optional>
#include <string>
#include <vector>

namespace asmodeus {

// An agent account with credentials and configuration.
struct AgentAccount {
  AgentAccount();
  ~AgentAccount();
  AgentAccount(const AgentAccount&);
  AgentAccount& operator=(const AgentAccount&);
  AgentAccount(AgentAccount&&);
  AgentAccount& operator=(AgentAccount&&);

  std::string name;         // "ultron"
  std::string email;        // "ultron.agent@ebm.ai"
  std::string password;     // Loaded from accounts.json
  std::string voice_model;  // "en_US-joe-medium"
  std::string profile;      // "~/.asmodeus/profiles/ultron"
  std::string totp_secret;  // Base32-encoded TOTP secret (empty = no TOTP)
};

// Reads agent credentials from ~/.asmodeus/accounts.json.
// The file is chmod 600 — only readable by the owner.
//
// Format:
//   {
//     "accounts": [
//       {"name": "ultron", "email": "...", "password": "...", "voiceModel": "..."}
//     ],
//     "phone": "+447539492403"
//   }
class CredentialStore {
 public:
  CredentialStore();
  ~CredentialStore();

  // Load credentials from path. Returns true if file parsed successfully.
  bool Load(const std::string& path);

  // Load from default path (~/.asmodeus/accounts.json).
  bool LoadDefault();

  // Look up account by agent name. Returns nullopt if not found.
  std::optional<AgentAccount> GetByName(const std::string& name) const;

  // Look up account by email. Returns nullopt if not found.
  std::optional<AgentAccount> GetByEmail(const std::string& email) const;

  // Get all accounts.
  const std::vector<AgentAccount>& accounts() const { return accounts_; }

  // Get the phone number for 2FA.
  const std::string& phone() const { return phone_; }

  // Number of accounts loaded.
  size_t size() const { return accounts_.size(); }

 private:
  std::vector<AgentAccount> accounts_;
  std::string phone_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_CREDENTIAL_STORE_H_
