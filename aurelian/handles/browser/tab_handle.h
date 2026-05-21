// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_HANDLES_BROWSER_TAB_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_TAB_HANDLE_H_

#include <cstdint>
#include <memory>

namespace content {
class WebContents;
}

namespace aurelian {

struct TabHandleImpl {
  void* handle_ptr = nullptr;
  int64_t tab_id = 0;
  ~TabHandleImpl();
};

struct TabsHandleImpl {
  void* handle_ptr = nullptr;
  ~TabsHandleImpl();
};

std::unique_ptr<TabHandleImpl> CreateTabHandle(content::WebContents* wc,
                                               int64_t tab_id);
void DestroyTabHandle(std::unique_ptr<TabHandleImpl> impl);
std::unique_ptr<TabsHandleImpl> CreateTabsHandle();

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_TAB_HANDLE_H_
