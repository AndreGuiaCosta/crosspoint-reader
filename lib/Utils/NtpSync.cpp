#include "NtpSync.h"

#include <Logging.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

bool NtpSync::syncTime(uint32_t timeoutMs) {
  // SNTP can't be reconfigured while running — stop any existing poll
  // before changing servers/mode.
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_init();

  constexpr uint32_t pollIntervalMs = 100;
  const uint32_t maxIterations = (timeoutMs + pollIntervalMs - 1) / pollIntervalMs;
  for (uint32_t i = 0; i < maxIterations; ++i) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      LOG_DBG("NTP", "Time synced after %u ms", static_cast<unsigned>(i * pollIntervalMs));
      return true;
    }
    vTaskDelay(pollIntervalMs / portTICK_PERIOD_MS);
  }

  LOG_DBG("NTP", "Sync timeout after %u ms", static_cast<unsigned>(timeoutMs));
  return false;
}
