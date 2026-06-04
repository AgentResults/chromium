// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Aurelian C9 — register the `chrome` facet INTO Agrippa over the local UDS
// (AURELIAN-DESIGN.md §3.6/§13, the SHIPPED machine-federation pattern). Unlike
// the bring-up WSS peer (wss_peer.h) — which LISTENS on a port — this is the
// canonical, de-socketed path: Aurelian opens NO inbound network socket. It
// DIALS Agrippa's generation-stable registration UDS (~/.legion/agrippa.sock),
// runs a substrate wire::Dispatcher whose slot-0 bootstrap resolves the sealed
// legion://chrome/ root, and sends the register handshake
//   update{kind:"Mount", name:"chrome",
//          type:"legion://types/RegisteredEmbodimentHandle", params:{childDestHash}}
// — the same UdsDispatcherLink::send_register frame Frontinus uses, routed by
// Agrippa's ChildEdge into register_embodiment. The hub then forwards a
// controller's leaf asks back over the UDS as dispatchAt{uri,verb,spec}.
//
// Velite types are pimpl-hidden so Chrome/test TUs stay Velite-free.

#ifndef AURELIAN_FEDERATION_UDS_REGISTER_H_
#define AURELIAN_FEDERATION_UDS_REGISTER_H_

#include <functional>
#include <memory>
#include <string>

namespace aurelian {

// Resolves a slash-path leaf ask against the legion://chrome/ root and returns
// the serialized reply — identical contract to RootDispatch / the WSS peer's
// DispatchFn (e.g. "system/info", "__getIdentity"). Production wires
// DispatchChromeRoot; tests pass a fake.
using ChromeDispatchFn = std::function<std::string(const std::string& path)>;

class UdsRegister {
 public:
  UdsRegister();
  ~UdsRegister();

  UdsRegister(const UdsRegister&) = delete;
  UdsRegister& operator=(const UdsRegister&) = delete;

  // Dial Agrippa's registration UDS at `socket_path`, serve `facet`
  // (e.g. "chrome") presenting `child_dest_hash` as the audit principal, and
  // send the update{Mount} register frame. `dispatch` resolves forwarded leaf
  // asks. Returns true iff the dial connected (the register frame was sent).
  // Spawns a serve thread that pumps inbound forwarded asks until Stop().
  bool Start(const std::string& socket_path,
             const std::string& facet,
             const std::string& child_dest_hash,
             ChromeDispatchFn dispatch);

  // Stop the serve loop, close the connection (the hub revokes the facet), join.
  void Stop();

  bool connected() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aurelian

#endif  // AURELIAN_FEDERATION_UDS_REGISTER_H_
