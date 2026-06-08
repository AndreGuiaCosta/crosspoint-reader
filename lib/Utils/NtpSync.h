#pragma once
#include <cstdint>

// SNTP wall-clock sync against pool.ntp.org. Required before TLS auth flows
// whose token validation loops on clock skew.
class NtpSync {
 public:
  // Block until the system clock is synced or timeoutMs elapses. Idempotent —
  // reconfigures any already-running SNTP poll.
  static bool syncTime(uint32_t timeoutMs = 5000);
};
