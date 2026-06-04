// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C11.c: page content info — MIME type + encoding (Page-domain read).

#ifndef AURELIAN_HANDLES_BROWSER_PAGE_INFO_HANDLE_H_
#define AURELIAN_HANDLES_BROWSER_PAGE_INFO_HANDLE_H_

#include <string>

namespace content {
class WebContents;
}

namespace aurelian {

struct PageContentInfo {
  std::string mime_type;
  std::string encoding;
};

// Reads the loaded document's content type + encoding. UI thread.
PageContentInfo GetPageContentInfo(content::WebContents* wc);

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_BROWSER_PAGE_INFO_HANDLE_H_
