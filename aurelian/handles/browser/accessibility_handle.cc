// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/browser/accessibility_handle.h"

#include "base/run_loop.h"
#include "content/public/browser/web_contents.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_mode.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/accessibility/ax_tree_update.h"

namespace aurelian {

std::vector<AxNode> SnapshotAxTree(content::WebContents* wc) {
  std::vector<AxNode> result;
  if (!wc) {
    return result;
  }

  base::RunLoop run_loop;
  wc->RequestAXTreeSnapshot(
      base::BindOnce(
          [](std::vector<AxNode>* out, base::RunLoop* loop,
             ui::AXTreeUpdate& update) {
            for (const ui::AXNodeData& node : update.nodes) {
              AxNode n;
              n.role = ui::ToString(node.role);
              n.name = node.GetStringAttribute(
                  ax::mojom::StringAttribute::kName);
              out->push_back(std::move(n));
            }
            loop->Quit();
          },
          &result, &run_loop),
      ui::kAXModeComplete, /*max_nodes=*/0,
      /*timeout=*/base::Seconds(0),
      content::WebContents::AXTreeSnapshotPolicy::kAll);
  run_loop.Run();
  return result;
}

}  // namespace aurelian
