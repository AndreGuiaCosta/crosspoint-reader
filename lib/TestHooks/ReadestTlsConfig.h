#pragma once

// WiFiClientSecure can't be forward-declared portably: the simulator declares
// it as `using WiFiClientSecure = NetworkClientSecure;` (an alias, not a
// class), so a `class WiFiClientSecure;` forward decl conflicts. Pulling in
// the full header costs nothing since every caller of this configure()
// function already needs it to instantiate the client.
#include <WiFiClientSecure.h>

/**
 * TLS configuration for Readest's HTTPS clients (`ReadestAuthClient`,
 * `ReadestSyncClient`).
 *
 * Production firmware (`src/test_hooks/ReadestTlsConfig.cpp`) installs the
 * Mozilla CA bundle that Arduino-ESP32 ships in flash — required for full
 * cert validation against `readest.com` (Let's Encrypt) and the Supabase
 * project hostname (ISRG Root X1/X2 chain).
 *
 * The simulator build excludes that `.cpp` via `build_src_filter` and the
 * `crosspoint-simulator` library provides a host-friendly implementation
 * that calls `setInsecure()` (the simulator's TLS verification is stubbed
 * out anyway, and the embedded cert bundle symbol doesn't exist on host).
 *
 * Header is identical in both builds.
 */
namespace ReadestTls {
void configure(WiFiClientSecure& client);
}  // namespace ReadestTls
