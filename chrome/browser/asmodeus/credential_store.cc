// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/credential_store.h"

#include <fstream>
#include <sstream>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/values.h"

namespace asmodeus {

AgentAccount::AgentAccount() = default;
AgentAccount::~AgentAccount() = default;
AgentAccount::AgentAccount(const AgentAccount&) = default;
AgentAccount& AgentAccount::operator=(const AgentAccount&) = default;
AgentAccount::AgentAccount(AgentAccount&&) = default;
AgentAccount& AgentAccount::operator=(AgentAccount&&) = default;

CredentialStore::CredentialStore() = default;
CredentialStore::~CredentialStore() = default;

bool CredentialStore::Load(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    LOG(WARNING) << "[Asmodeus] CredentialStore: cannot open " << path;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  auto parsed = base::JSONReader::Read(content,
      base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    LOG(ERROR) << "[Asmodeus] CredentialStore: invalid JSON in " << path;
    return false;
  }

  const auto& dict = parsed->GetDict();

  // Read phone number
  if (const auto* phone = dict.FindString("phone")) {
    phone_ = *phone;
  }

  // Read accounts
  const auto* accounts_list = dict.FindList("accounts");
  if (!accounts_list) {
    LOG(WARNING) << "[Asmodeus] CredentialStore: no 'accounts' array in "
                 << path;
    return false;
  }

  accounts_.clear();
  for (const auto& item : *accounts_list) {
    if (!item.is_dict()) continue;
    const auto& acct = item.GetDict();

    AgentAccount account;
    if (const auto* name = acct.FindString("name"))
      account.name = *name;
    if (const auto* email = acct.FindString("email"))
      account.email = *email;
    if (const auto* password = acct.FindString("password"))
      account.password = *password;
    if (const auto* voice = acct.FindString("voiceModel"))
      account.voice_model = *voice;
    if (const auto* totp = acct.FindString("totpSecret"))
      account.totp_secret = *totp;
    if (const auto* profile = acct.FindString("profile"))
      account.profile = *profile;

    if (account.name.empty() || account.email.empty()) {
      LOG(WARNING) << "[Asmodeus] CredentialStore: skipping account with "
                   << "missing name or email";
      continue;
    }

    // Expand ~ in profile path
    if (!account.profile.empty() && account.profile[0] == '~') {
      const char* home = getenv("HOME");
      if (home) {
        account.profile = std::string(home) + account.profile.substr(1);
      }
    }

    accounts_.push_back(std::move(account));
  }

  LOG(INFO) << "[Asmodeus] CredentialStore: loaded " << accounts_.size()
            << " accounts from " << path;
  return true;
}

bool CredentialStore::LoadDefault() {
  const char* home = getenv("HOME");
  if (!home) return false;
  return Load(std::string(home) + "/.asmodeus/accounts.json");
}

std::optional<AgentAccount> CredentialStore::GetByName(
    const std::string& name) const {
  for (const auto& acct : accounts_) {
    if (acct.name == name) return acct;
  }
  return std::nullopt;
}

std::optional<AgentAccount> CredentialStore::GetByEmail(
    const std::string& email) const {
  for (const auto& acct : accounts_) {
    if (acct.email == email) return acct;
  }
  return std::nullopt;
}

}  // namespace asmodeus
