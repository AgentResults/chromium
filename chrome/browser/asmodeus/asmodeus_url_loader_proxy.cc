// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/asmodeus_url_loader_proxy.h"

#include "base/logging.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/net_errors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace asmodeus {

AsmodeusURLLoaderProxy::AsmodeusURLLoaderProxy(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
    : target_(std::move(target)) {
  receivers_.Add(this, std::move(receiver));
  receivers_.set_disconnect_handler(
      base::BindRepeating(&AsmodeusURLLoaderProxy::OnDisconnect,
                          base::Unretained(this)));
}

AsmodeusURLLoaderProxy::~AsmodeusURLLoaderProxy() = default;

void AsmodeusURLLoaderProxy::CreateLoaderAndStart(
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    int32_t request_id,
    uint32_t options,
    const network::ResourceRequest& request,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation) {
  // Forward all requests to the real factory — including reCAPTCHA.
  // reCAPTCHA must run to generate a valid token for Meet's join flow.
  target_->CreateLoaderAndStart(std::move(loader), request_id, options,
                                request, std::move(client),
                                traffic_annotation);
}

void AsmodeusURLLoaderProxy::Clone(
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver) {
  receivers_.Add(this, std::move(receiver));
}

void AsmodeusURLLoaderProxy::OnDisconnect() {
  if (receivers_.empty()) {
    delete this;
  }
}

}  // namespace asmodeus
