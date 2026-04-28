#pragma once
#include <cstdint>

/**
 * Shared SNTP utility used by sync clients (KOReader, Readest) before
 * making network requests that depend on a correct wall-clock — token
 * refresh logic in particular loops on clock skew.
 *
 * On real hardware this drives the ESP-IDF SNTP daemon against
 * pool.ntp.org. In the simulator the underlying esp_sntp_* shims
 * report immediate completion so this is effectively a no-op.
 */
class NtpSync {
 public:
  /**
   * Block until the system clock is synced or the timeout elapses.
   * Idempotent — stops and reconfigures any already-running SNTP poll.
   *
   * @param timeoutMs maximum time to wait, default 5 s.
   * @return true if sync completed within the timeout, false on timeout.
   */
  static bool syncTime(uint32_t timeoutMs = 5000);
};
