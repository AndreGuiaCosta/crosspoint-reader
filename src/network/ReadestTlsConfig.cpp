#include <ReadestTlsConfig.h>
#include <WiFiClientSecure.h>

// Use the CA bundle baked into pioarduino's framework-libs (libmbedtls.a
// ships x509_crt_bundle.S.obj — ~150 Mozilla roots). A locally generated
// bundle was rejected by this framework build's verify_callback, so stick
// with the framework default.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace ReadestTls {
void configure(WiFiClientSecure& client) {
  client.setCACertBundle(rootca_crt_bundle_start, static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
}
}  // namespace ReadestTls
