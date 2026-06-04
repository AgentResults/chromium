// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/capability/cap_anchor_provisioner.h"

#include <utility>

#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"

namespace aurelian {

CapAnchorProvisioner::CapAnchorProvisioner(content::WebContents* web_contents,
                                           std::vector<uint8_t> anchor_pub)
    : content::WebContentsObserver(web_contents),
      anchor_pub_(std::move(anchor_pub)) {
  // Provision the already-live primary main frame (the tab usually exists with
  // an about:blank frame before this observer attaches); future frames are
  // caught by RenderFrameCreated.
  if (web_contents->GetPrimaryMainFrame()) {
    Provision(web_contents->GetPrimaryMainFrame());
  }
}

CapAnchorProvisioner::~CapAnchorProvisioner() = default;

void CapAnchorProvisioner::RenderFrameCreated(
    content::RenderFrameHost* render_frame_host) {
  Provision(render_frame_host);
}

void CapAnchorProvisioner::RenderFrameDeleted(
    content::RenderFrameHost* render_frame_host) {
  wires_.erase(render_frame_host);
}

void CapAnchorProvisioner::Provision(
    content::RenderFrameHost* render_frame_host) {
  if (anchor_pub_.size() != 32) {
    return;
  }
  mojo::AssociatedRemote<mojom::AurelianWire> wire;
  render_frame_host->GetRemoteAssociatedInterfaces()->GetInterface(&wire);
  wire->SetTrustAnchor(anchor_pub_);
  // Keep the remote alive so the fire-and-forget SetTrustAnchor is delivered
  // before the pipe would otherwise close.
  wires_[render_frame_host] = std::move(wire);
}

}  // namespace aurelian
