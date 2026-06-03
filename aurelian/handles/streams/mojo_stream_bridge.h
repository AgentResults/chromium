// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_HANDLES_STREAMS_MOJO_STREAM_BRIDGE_H_
#define AURELIAN_HANDLES_STREAMS_MOJO_STREAM_BRIDGE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "aurelian/public/mojom/aurelian_wire.mojom.h"
#include "base/functional/callback.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace aurelian {

// Browser-side end of a renderer subscription (C6.b). Opens a Subscribe over
// a frame's AurelianWire, binds itself as the VeliteSink, and forwards every
// Frame to `on_frame`. Destroying the bridge drops the VeliteSink receiver,
// which the renderer producer treats as cancel.
class MojoStreamBridge : public aurelian::mojom::VeliteSink {
 public:
  using FrameCallback =
      base::RepeatingCallback<void(const std::vector<uint8_t>&)>;

  explicit MojoStreamBridge(FrameCallback on_frame);
  ~MojoStreamBridge() override;

  MojoStreamBridge(const MojoStreamBridge&) = delete;
  MojoStreamBridge& operator=(const MojoStreamBridge&) = delete;

  // Opens the subscription for `stream` over `wire`. `wire` must outlive the
  // call (the request is sent synchronously).
  void Subscribe(mojo::AssociatedRemote<aurelian::mojom::AurelianWire>& wire,
                 const std::string& stream);

  // aurelian::mojom::VeliteSink:
  void Frame(const std::vector<uint8_t>& frame) override;

 private:
  void OnReceiver(mojo::PendingReceiver<aurelian::mojom::VeliteSink> receiver);

  FrameCallback on_frame_;
  mojo::Receiver<aurelian::mojom::VeliteSink> receiver_{this};
};

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_STREAMS_MOJO_STREAM_BRIDGE_H_
