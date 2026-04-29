#pragma once
#include <string>

namespace ReadestAuthRefresh {
// Refresh the access token. Returns true iff the store now holds a usable
// one. On failure, populates *errMsg if the caller passed an empty one and
// the refresh produced detail.
bool refresh(const char* tag, std::string* errMsg);
}  // namespace ReadestAuthRefresh
