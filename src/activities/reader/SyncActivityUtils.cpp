#include "SyncActivityUtils.h"

#include <BluetoothHIDManager.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>

namespace SyncActivityUtils {

void releaseHeapForTls(GfxRenderer& renderer) {
  LOG_INF("Sync", "Releasing heap for TLS (free before: %u)", (unsigned)ESP.getFreeHeap());

  auto& btMgr = BluetoothHIDManager::getInstance();
  if (btMgr.isEnabled()) {
    btMgr.disable();
    btMgr.requestEnableLater();
  }

  // SD .cpfont glyph caches survive the reader session (and with no BT
  // bonded, prewarm was eager for all styles). Glyphs reload on demand.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }

  LOG_INF("Sync", "Heap released for TLS (free after: %u)", (unsigned)ESP.getFreeHeap());
}

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
