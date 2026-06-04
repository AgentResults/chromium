// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_CAPABILITY_CAP_ANCHOR_PROVISIONER_H_
#define AURELIAN_CAPABILITY_CAP_ANCHOR_PROVISIONER_H_

#include <cstdint>
#include <map>
#include <vector>

#include "aurelian/public/mojom/aurelian_wire.mojom.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/associated_remote.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace aurelian {

// CapAnchorProvisioner — provisions the operator/machine cap trust anchor onto
// every renderer frame's AurelianWire membrane as the frame is created. This is
// the LIVE-boot half of C8 that cap_membrane_browsertest only simulated
// (ProvisionAnchor "what the browser would do once at boot from the
// Agrippa-published key"). Without it, the renderer membrane's `has_anchor_`
// stays false and EnforceCap fail-closes EVERY cap-gated op to
// `cap-untrusted-anchor` — i.e. the whole attenuation layer is inert in
// production. One provisioner per WebContents; it keeps each frame's wire remote
// alive so the SetTrustAnchor message is delivered.
class CapAnchorProvisioner : public content::WebContentsObserver {
 public:
  // `anchor_pub` is the 32-byte Ed25519 operator anchor. A wrong-sized anchor
  // makes this a no-op (the membrane simply never gains an anchor).
  CapAnchorProvisioner(content::WebContents* web_contents,
                       std::vector<uint8_t> anchor_pub);
  ~CapAnchorProvisioner() override;

  CapAnchorProvisioner(const CapAnchorProvisioner&) = delete;
  CapAnchorProvisioner& operator=(const CapAnchorProvisioner&) = delete;

  // content::WebContentsObserver:
  void RenderFrameCreated(content::RenderFrameHost* render_frame_host) override;
  void RenderFrameDeleted(content::RenderFrameHost* render_frame_host) override;

 private:
  void Provision(content::RenderFrameHost* render_frame_host);

  const std::vector<uint8_t> anchor_pub_;
  std::map<content::RenderFrameHost*,
           mojo::AssociatedRemote<mojom::AurelianWire>>
      wires_;
};

}  // namespace aurelian

#endif  // AURELIAN_CAPABILITY_CAP_ANCHOR_PROVISIONER_H_
