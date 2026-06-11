#pragma once
#include <Logging.h>

#include <string>

#include "ReadestAuthRefresh.h"

namespace ReadestAuthRetry {

// Run an authenticated request; on AUTH_EXPIRED refresh the session once and
// retry. `authExpired` is the calling client's AUTH_EXPIRED enumerator (the
// sync and storage clients have distinct Error enums). errMsg is cleared
// before the retry so a stale refresh-path message can't shadow the retry's
// own outcome.
template <typename Error, typename Call>
Error run(const char* tag, Error authExpired, std::string* errMsg, Call&& call) {
  Error rc = call();
  if (rc != authExpired) return rc;
  LOG_DBG("RRTRY", "%s AUTH_EXPIRED, attempting refresh", tag);
  if (!ReadestAuthRefresh::refresh(tag, errMsg)) return authExpired;
  if (errMsg) errMsg->clear();
  return call();
}

}  // namespace ReadestAuthRetry
