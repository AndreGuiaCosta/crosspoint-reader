#pragma once
#include <cstdint>
#include <string>

namespace ReadestTimeUtils {
// Parse `YYYY-MM-DDTHH:MM:SS[.fff][Z|±HH:MM]` (UTC) → unix ms. Returns 0 on
// any parse failure. Uses Howard Hinnant's days_from_civil so we don't rely
// on platform timegm().
int64_t parseIso8601ToMs(const std::string& iso);
}  // namespace ReadestTimeUtils
