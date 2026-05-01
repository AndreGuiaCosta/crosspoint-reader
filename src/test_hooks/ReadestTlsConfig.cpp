#include <ReadestTlsConfig.h>
#include <WiFiClientSecure.h>

// Use the CA bundle that ships baked into pioarduino's framework-libs
// (libmbedtls.a contains x509_crt_bundle.S.obj — ~150 Mozilla roots,
// including GTS Root R4 used by readest.com's Cloudflare/Supabase chain).
//
// We previously embedded our own bundle generated from certifi via
// gen_crt_bundle.py, but the runtime in this framework build appears to
// reject that variant for reasons we couldn't pin down without serial
// access — verify_callback returned MBEDTLS_ERR_X509_FATAL_ERROR (-0x3000),
// which IDF's esp_crt_bundle.c only emits when s_crt_bundle is NULL after
// init. The framework default is known-good and sidesteps that path.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace ReadestTls {
void configure(WiFiClientSecure& client) {
  client.setCACertBundle(rootca_crt_bundle_start, static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
}
}  // namespace ReadestTls
