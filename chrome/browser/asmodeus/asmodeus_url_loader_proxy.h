// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_ASMODEUS_URL_LOADER_PROXY_H_
#define CHROME_BROWSER_ASMODEUS_ASMODEUS_URL_LOADER_PROXY_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace asmodeus {

// Proxying URL loader factory that blocks reCAPTCHA requests for
// Asmodeus participant frames. Intercepts all subresource loads
// (scripts, iframes, images) and cancels any to recaptcha domains.
//
// This is the proper Chromium pattern for URL filtering — used by
// extensions WebRequestAPI, Safe Browsing, etc.
class AsmodeusURLLoaderProxy : public network::mojom::URLLoaderFactory {
 public:
  AsmodeusURLLoaderProxy(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target);
  ~AsmodeusURLLoaderProxy() override;

  AsmodeusURLLoaderProxy(const AsmodeusURLLoaderProxy&) = delete;
  AsmodeusURLLoaderProxy& operator=(const AsmodeusURLLoaderProxy&) = delete;

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override;
  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      override;

 private:
  void OnDisconnect();

  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_ASMODEUS_URL_LOADER_PROXY_H_
