// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AURELIAN_HANDLES_STREAMS_SUBSCRIPTION_PRODUCER_H_
#define AURELIAN_HANDLES_STREAMS_SUBSCRIPTION_PRODUCER_H_

#include <memory>
#include <string>
#include <vector>

namespace velite::agentspaces {
class Handle;
class Value;
class SubscriptionHandle;
}  // namespace velite::agentspaces

namespace aurelian {

// Local (in-process) subscription fan-out (C6.a). A producer owns a set of
// velite::agentspaces::SubscriptionHandle, each bound to a `sink` Handle.
// Broadcast() pushes one frame to every ACTIVE subscription as a
// `legion-notify` tell, and prunes subscriptions the holder cancelled. This
// is the substrate-local half of the streams design; C6.b carries the same
// frame contract across the Mojo VeliteSink pipe.
class SubscriptionProducer {
 public:
  SubscriptionProducer();
  ~SubscriptionProducer();

  SubscriptionProducer(const SubscriptionProducer&) = delete;
  SubscriptionProducer& operator=(const SubscriptionProducer&) = delete;

  // Mints a SubscriptionHandle for `sink` under `uri_prefix` (the producer's
  // stream URI). `filter` is an opaque match string (empty = all frames).
  // Returns the handle so the caller can read getState / tell("cancel").
  std::shared_ptr<velite::agentspaces::Handle> Subscribe(
      std::shared_ptr<velite::agentspaces::Handle> sink,
      const std::string& uri_prefix,
      const std::string& filter);

  // Delivers `frame` to every active subscription; cancelled/failed ones are
  // dropped.
  void Broadcast(const velite::agentspaces::Value& frame);

  // Number of still-active subscriptions.
  size_t active_count() const;

 private:
  int next_id_ = 1;
  std::vector<std::shared_ptr<velite::agentspaces::SubscriptionHandle>> subs_;
};

}  // namespace aurelian

#endif  // AURELIAN_HANDLES_STREAMS_SUBSCRIPTION_PRODUCER_H_
