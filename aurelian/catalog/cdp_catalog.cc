// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/catalog/cdp_catalog.h"

#include "aurelian/catalog/embedded_catalog_data.h"
#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"

namespace aurelian {

namespace {

base::DictValue ParseEmbedded(const char* json, const char* what) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  CHECK(parsed.has_value() && parsed->is_dict())
      << "embedded catalog data is not a JSON dict: " << what;
  return std::move(*parsed).TakeDict();
}

}  // namespace

CdpCatalog::~CdpCatalog() = default;

// static
const CdpCatalog& CdpCatalog::Get() {
  static const base::NoDestructor<CdpCatalog> instance;
  return *instance;
}

CdpCatalog::CdpCatalog()
    : descriptor_(ParseEmbedded(kCdpProtocolJson, "protocol.json")),
      table_(ParseEmbedded(kSessionContextJson, "session_context_table")),
      overrides_(ParseEmbedded(kCommandOverridesJson, "command_overrides")) {
  const base::ListValue* domain_list = descriptor_.FindList("domains");
  CHECK(domain_list) << "descriptor carries no domains list";
  for (const base::Value& d : *domain_list) {
    const base::DictValue* dd = d.GetIfDict();
    CHECK(dd);
    const std::string* name = dd->FindString("domain");
    CHECK(name);
    domains_.push_back(*name);
    if (const base::ListValue* cmds = dd->FindList("commands")) {
      command_count_ += cmds->size();
    }
    if (const base::ListValue* evts = dd->FindList("events")) {
      event_count_ += evts->size();
    }
  }
}

const base::DictValue* CdpCatalog::FindCommand(
    const std::string& domain,
    const std::string& command) const {
  const base::ListValue* domain_list = descriptor_.FindList("domains");
  for (const base::Value& d : *domain_list) {
    const base::DictValue& dd = d.GetDict();
    const std::string* name = dd.FindString("domain");
    if (!name || *name != domain) {
      continue;
    }
    const base::ListValue* cmds = dd.FindList("commands");
    if (!cmds) {
      return nullptr;
    }
    for (const base::Value& c : *cmds) {
      const base::DictValue& cd = c.GetDict();
      const std::string* cname = cd.FindString("name");
      if (cname && *cname == command) {
        return &cd;
      }
    }
    return nullptr;
  }
  return nullptr;
}

std::optional<std::string> CdpCatalog::FirstCommandOf(
    const std::string& domain) const {
  const base::ListValue* domain_list = descriptor_.FindList("domains");
  for (const base::Value& d : *domain_list) {
    const base::DictValue& dd = d.GetDict();
    const std::string* name = dd.FindString("domain");
    if (!name || *name != domain) {
      continue;
    }
    const base::ListValue* cmds = dd.FindList("commands");
    if (!cmds || cmds->empty()) {
      return std::nullopt;
    }
    const std::string* cname = cmds->front().GetDict().FindString("name");
    return cname ? std::optional<std::string>(*cname) : std::nullopt;
  }
  return std::nullopt;
}

std::optional<CdpCatalog::ContextRow> CdpCatalog::ContextFor(
    const std::string& domain) const {
  const base::DictValue* rows = table_.FindDict("domains");
  CHECK(rows);
  const base::DictValue* row = rows->FindDict(domain);
  if (!row) {
    return std::nullopt;
  }
  ContextRow out;
  out.in_browser_union = row->FindBool("inBrowserUnion").value_or(false);
  out.browser_only = row->FindBool("browserOnly").value_or(false);
  out.conditional = row->FindBool("conditional").value_or(false);
  return out;
}

std::vector<std::string> CdpCatalog::BrowserOnlyDomains() const {
  std::vector<std::string> out;
  for (const std::string& d : domains_) {
    std::optional<ContextRow> row = ContextFor(d);
    if (row && row->browser_only) {
      out.push_back(d);
    }
  }
  return out;
}

std::optional<std::string> CdpCatalog::OverrideContextFor(
    const std::string& qualified_command) const {
  const base::ListValue* rows = overrides_.FindList("overrides");
  CHECK(rows);
  for (const base::Value& r : *rows) {
    const base::DictValue& rd = r.GetDict();
    const std::string* cmd = rd.FindString("command");
    if (cmd && *cmd == qualified_command) {
      const std::string* ctx = rd.FindString("context");
      return ctx ? std::optional<std::string>(*ctx) : std::nullopt;
    }
  }
  return std::nullopt;
}

std::vector<std::string> CdpCatalog::OverrideCommands() const {
  std::vector<std::string> out;
  const base::ListValue* rows = overrides_.FindList("overrides");
  CHECK(rows);
  for (const base::Value& r : *rows) {
    const std::string* cmd = r.GetDict().FindString("command");
    if (cmd) {
      out.push_back(*cmd);
    }
  }
  return out;
}

}  // namespace aurelian
