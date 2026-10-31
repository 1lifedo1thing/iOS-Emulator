// The guest's web proxy, one per running device (it runs in the worker).
//
// Prepared devices carry a proxy auto-config file (firmwarekit's, /usr/share/ltm/proxy.pac) that
// sends iOS's web traffic to 10.0.2.100:3128 first. The engine forwards that address to this
// proxy's Unix socket, so it always answers at once (nothing listening there made every new
// connection wait for a timeout before iOS went direct).
//
// What it does with a request depends on the mode:
//   Off       the device's internet is off: every request is refused.
//   Direct    bytes pass through untouched, as if iOS had connected itself.
//   Bridge    HTTPS is re-encrypted: iOS speaks its old TLS to the proxy (which it trusts through a
//             certificate installed in the guest), and the proxy speaks modern TLS to the website,
//             checking the site's certificate with Windows' own trust store. Old iOS can't open most
//             of today's HTTPS sites without it.
// Destinations on your own computer or local network are always refused.
#pragma once

#include "common.h"

#include <memory>

class WebProxy {
public:
    enum class Mode { Off, Direct, Bridge };

    // Loads the device's proxy identity (a CA key and certificate in `identityDir`, kept across
    // sessions so the guest keeps trusting it) or creates one. Throws if that fails.
    explicit WebProxy(const fs::path &identityDir);
    ~WebProxy();
    WebProxy(const WebProxy &) = delete;
    WebProxy &operator=(const WebProxy &) = delete;

    // Starts accepting connections on a Unix socket (the engine's guestfwd target).
    void Listen(const fs::path &socketPath);

    // Applies to connections made from now on; open ones keep their mode.
    void SetMode(Mode mode);

    std::string Certificate() const;   // the CA certificate (DER), for the guest's trust store

private:
    struct State;
    std::shared_ptr<State> state_;
};
