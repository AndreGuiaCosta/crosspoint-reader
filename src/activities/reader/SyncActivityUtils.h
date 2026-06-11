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

// Metadata-only Epub load for progress mapping (no CSS, no cache rebuild —
// keeps the reload cheap on a post-TLS heap). Returns null on failure.
std::shared_ptr<Epub> loadEpubForSync(const std::string& epubPath);

}  // namespace SyncActivityUtils
