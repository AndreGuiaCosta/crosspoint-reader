#pragma once
#include <Epub.h>

#include <memory>
#include <string>

// Helpers shared by the KOReader and Readest sync activities — the WiFi
// teardown sequencing and the lightweight Epub reload both encode hard-won
// heap behaviour that must not drift between the two flows.
namespace SyncActivityUtils {

// Stop SNTP and power WiFi fully down (both sync flows end this way).
void wifiOff();

// Drop the NimBLE host before a TLS handshake — it holds 50-68KB and the
// mbedTLS session needs ~55KB free. Pairs disable() with requestEnableLater()
// so the main loop reconnects the remote once the network work is done.
// No-op when Bluetooth is off.
void releaseBleForTls();

// Metadata-only Epub load for progress mapping (no CSS, no cache rebuild —
// keeps the reload cheap on a post-TLS heap). Returns null on failure.
std::shared_ptr<Epub> loadEpubForSync(const std::string& epubPath);

}  // namespace SyncActivityUtils
