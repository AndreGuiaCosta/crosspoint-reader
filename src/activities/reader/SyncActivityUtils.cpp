#include "SyncActivityUtils.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

namespace SyncActivityUtils {

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

std::shared_ptr<Epub> loadEpubForSync(const std::string& epubPath) {
  LOG_DBG("Sync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
  auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
  if (!epub->load(false, true)) {
    LOG_ERR("Sync", "Failed to load epub for progress mapping");
    return nullptr;
  }
  LOG_DBG("Sync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  return epub;
}

}  // namespace SyncActivityUtils
