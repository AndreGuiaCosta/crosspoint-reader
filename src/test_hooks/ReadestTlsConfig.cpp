#include <ReadestTlsConfig.h>
#include <WiFiClientSecure.h>

// Mozilla CA bundle baked into Arduino-ESP32. The linker exposes its start
// address under this symbol; passing it to setCACertBundle enables full chain
// validation against well-known CAs (Let's Encrypt for readest.com, the
// Supabase chain via ISRG Root X1/X2).
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_bin_start");

namespace ReadestTls {
void configure(WiFiClientSecure& client) { client.setCACertBundle(rootca_crt_bundle_start); }
}  // namespace ReadestTls
