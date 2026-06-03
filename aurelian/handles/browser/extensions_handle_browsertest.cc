// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C7.b browser tests — extensions list / enable / disable.

#include "aurelian/handles/browser/extensions_handle.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "extensions/browser/extension_registrar.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_builder.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace aurelian {

class AurelianExtBrowserTest : public InProcessBrowserTest {
 protected:
  content::BrowserContext* Ctx() { return browser()->profile(); }

  // Builds a trivial extension and installs it (enabled). Returns its id.
  std::string AddTestExtension(const std::string& name) {
    scoped_refptr<const extensions::Extension> ext =
        extensions::ExtensionBuilder(name).Build();
    std::string id = ext->id();
    extensions::ExtensionRegistrar::Get(Ctx())->AddExtension(ext);
    return id;
  }

  bool InList(const std::vector<ExtensionInfo>& list, const std::string& id,
              bool* enabled_out) {
    for (const auto& e : list) {
      if (e.id == id) {
        if (enabled_out) *enabled_out = e.enabled;
        return true;
      }
    }
    return false;
  }
};

IN_PROC_BROWSER_TEST_F(AurelianExtBrowserTest, ListAndToggle) {
  std::string id = AddTestExtension("Aurelian Test Ext");
  // Precondition: the extension really installed enabled.
  ASSERT_TRUE(extensions::ExtensionRegistry::Get(Ctx())
                  ->enabled_extensions()
                  .Contains(id));

  // list shows it, enabled.
  bool enabled = false;
  ASSERT_TRUE(InList(ListExtensions(Ctx()), id, &enabled))
      << "installed extension missing from list";
  EXPECT_TRUE(enabled);

  // disable.
  EXPECT_TRUE(DisableExtensionById(Ctx(), id));
  EXPECT_TRUE(extensions::ExtensionRegistry::Get(Ctx())
                  ->disabled_extensions()
                  .Contains(id));
  ASSERT_TRUE(InList(ListExtensions(Ctx()), id, &enabled));
  EXPECT_FALSE(enabled) << "extension still reported enabled after disable";

  // enable.
  EXPECT_TRUE(EnableExtensionById(Ctx(), id));
  EXPECT_TRUE(extensions::ExtensionRegistry::Get(Ctx())
                  ->enabled_extensions()
                  .Contains(id));
  ASSERT_TRUE(InList(ListExtensions(Ctx()), id, &enabled));
  EXPECT_TRUE(enabled);

  // toggling a non-existent id fails.
  EXPECT_FALSE(DisableExtensionById(Ctx(), "00000000000000000000000000000000"));
}

}  // namespace aurelian
