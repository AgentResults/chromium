// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aurelian/handles/streams/mojo_stream_bridge.h"

#include <utility>

#include "base/functional/bind.h"

namespace aurelian {

MojoStreamBridge::MojoStreamBridge(FrameCallback on_frame)
    : on_frame_(std::move(on_frame)) {}

MojoStreamBridge::~MojoStreamBridge() = default;

void MojoStreamBridge::Subscribe(
    mojo::AssociatedRemote<aurelian::mojom::AurelianWire>& wire,
    const std::string& stream) {
  std::vector<uint8_t> envelope(stream.begin(), stream.end());
  wire->Subscribe(envelope, base::BindOnce(&MojoStreamBridge::OnReceiver,
                                           base::Unretained(this)));
}

void MojoStreamBridge::OnReceiver(
    mojo::PendingReceiver<aurelian::mojom::VeliteSink> receiver) {
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void MojoStreamBridge::Frame(const std::vector<uint8_t>& frame) {
  if (on_frame_) {
    on_frame_.Run(frame);
  }
}

}  // namespace aurelian
