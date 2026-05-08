#include "ReadestAuthRefresh.h"

#include <Logging.h>

#include "ReadestAuthClient.h"

bool ReadestAuthRefresh::refresh(const char* tag, std::string* errMsg) {
  std::string refreshErr;
  const auto rc = ReadestAuthClient::refresh(&refreshErr);
  if (rc == ReadestAuthClient::OK) {
    LOG_DBG("RAR", "%s refreshed token", tag);
    return true;
  }
  LOG_ERR("RAR", "%s refresh failed: %s (%s)", tag, ReadestAuthClient::errorString(rc), refreshErr.c_str());
  if (errMsg && errMsg->empty()) *errMsg = refreshErr;
  return false;
}
