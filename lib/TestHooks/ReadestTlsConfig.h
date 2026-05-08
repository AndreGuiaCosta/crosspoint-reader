#pragma once

// Pulled in via full include; can't forward-declare portably because the
// simulator aliases WiFiClientSecure to NetworkClientSecure.
#include <WiFiClientSecure.h>

// Configures TLS for the Readest HTTPS clients. Firmware installs the CA
// bundle; the simulator build links a host-friendly setInsecure() variant.
namespace ReadestTls {
void configure(WiFiClientSecure& client);
}  // namespace ReadestTls
