#pragma once
#include <Epub.h>

#include <memory>
#include <string>

class GfxRenderer;

// Helpers shared by the KOReader and Readest sync activities — the WiFi
// teardown sequencing and the lightweight Epub reload both encode hard-won
// heap behaviour that must not drift between the two flows.
namespace SyncActivityUtils {

// Stop SNTP and power WiFi fully down (both sync flows end this way).
void wifiOff();

// Free the big transient residents before a TLS handshake — mbedTLS needs
// ~55KB and the C3 doesn't have that to spare while they're held:
// - NimBLE host (50-68KB): disable() + requestEnableLater(), so the main
//   loop reconnects the remote once the network work is done. No-op when
//   Bluetooth is off.
// - Font glyph caches (tens of KB with an SD .cpfont after a reader
//   session): cleared; glyphs reload on demand at the next render.
void releaseHeapForTls(GfxRenderer& renderer);

// Metadata-only Epub load for progress mapping (no CSS, no cache rebuild —
// keeps the reload cheap on a post-TLS heap). Returns null on failure.
std::shared_ptr<Epub> loadEpubForSync(const std::string& epubPath);

}  // namespace SyncActivityUtils
