#include "ReadestSyncClient.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cstdio>

#include "ReadestAccountStore.h"
#include "ReadestHttp.h"
#include "ReadestTimeUtils.h"

namespace {
constexpr int SYNC_CONNECT_TIMEOUT = 5000;
constexpr int SYNC_READ_TIMEOUT = 15000;

ReadestSyncClient::Error mapHttpStatus(int code) {
  using E = ReadestSyncClient::Error;
  if (code == 200) return E::OK;
  if (code == 400) return E::BAD_REQUEST;
  if (code == 401 || code == 403) return E::AUTH_EXPIRED;
  if (code >= 500) return E::SERVER_ERROR;
  if (code < 0) return E::NETWORK_ERROR;
  return E::SERVER_ERROR;
}

bool parseProgressString(const std::string& s, int* cur, int* total) {
  if (s.empty()) return false;
  return std::sscanf(s.c_str(), " [ %d , %d ]", cur, total) == 2;
}

void rowToConfig(JsonObjectConst row, ReadestSyncClient::BookConfig& out) {
  out.bookHash = row["book_hash"] | std::string("");
  out.metaHash = row["meta_hash"] | std::string("");
  out.xpointer = row["xpointer"] | std::string("");
  out.location = row["location"] | std::string("");

  const std::string progressStr = row["progress"] | std::string("");
  parseProgressString(progressStr, &out.progressCurrent, &out.progressTotal);

  const std::string updatedAt = row["updated_at"] | std::string("");
  out.updatedAtMs = ReadestTimeUtils::parseIso8601ToMs(updatedAt);

  const std::string deletedAt = row["deleted_at"] | std::string("");
  out.deleted = !deletedAt.empty();
}
}  // namespace

ReadestSyncClient::Error ReadestSyncClient::pullConfig(int64_t sinceMs, const std::string& bookHash,
                                                       const std::string& metaHash, BookConfig* out,
                                                       int64_t* maxUpdatedAtMs, std::string* errMsg) {
  if (out) *out = BookConfig{};
  if (maxUpdatedAtMs) *maxUpdatedAtMs = 0;

  const std::string accessToken = READEST_STORE.getAccessToken();
  if (accessToken.empty()) {
    LOG_DBG("RSYNC", "pull: no access token");
    return NO_AUTH;
  }

  char sinceBuf[32];
  std::snprintf(sinceBuf, sizeof(sinceBuf), "%lld", static_cast<long long>(sinceMs));

  ReadestHttp::Request rq;
  rq.tag = "RSYNC";
  rq.url = READEST_STORE.getSyncApiBase() + "/sync?since=" + sinceBuf + "&type=configs&book=" + bookHash +
           "&meta_hash=" + metaHash;
  rq.headers = ReadestHttp::bearerHeaders(accessToken);
  rq.connectTimeoutMs = SYNC_CONNECT_TIMEOUT;
  rq.readTimeoutMs = SYNC_READ_TIMEOUT;

  JsonDocument doc;
  const int code = ReadestHttp::requestJson(rq, &doc, errMsg);
  if (code == ReadestHttp::JSON_PARSE_FAILED) return JSON_ERROR;
  if (code != 200) return mapHttpStatus(code);

  // Server filter unions book_hash and meta_hash matches (spec §5.3: a row
  // stored under a byte-different file of the same conceptual book matches
  // via meta_hash only). Prefer an exact book_hash row; otherwise accept the
  // newest meta_hash row. Track maxUpdatedAtMs across all rows so the cursor
  // advances even when the match is older than a sibling row.
  JsonArrayConst configs = doc["configs"];
  bool haveExact = false;
  for (JsonObjectConst row : configs) {
    BookConfig parsed;
    rowToConfig(row, parsed);
    if (maxUpdatedAtMs && parsed.updatedAtMs > *maxUpdatedAtMs) {
      *maxUpdatedAtMs = parsed.updatedAtMs;
    }
    if (!out) continue;
    const bool exact = !bookHash.empty() && parsed.bookHash == bookHash;
    const bool metaOnly = !exact && !metaHash.empty() && parsed.metaHash == metaHash;
    if (exact && !haveExact) {
      haveExact = true;
      *out = std::move(parsed);
    } else if (metaOnly && !haveExact && parsed.updatedAtMs > out->updatedAtMs) {
      *out = std::move(parsed);
    }
  }
  return OK;
}

ReadestSyncClient::Error ReadestSyncClient::pushConfig(const BookConfig& cfg, BookConfig* outAuthoritative,
                                                       std::string* errMsg) {
  if (outAuthoritative) *outAuthoritative = BookConfig{};
  const std::string accessToken = READEST_STORE.getAccessToken();
  if (accessToken.empty()) {
    LOG_DBG("RSYNC", "push: no access token");
    return NO_AUTH;
  }

  JsonDocument req;
  JsonArray configs = req["configs"].to<JsonArray>();
  JsonObject c = configs.add<JsonObject>();
  c["bookHash"] = cfg.bookHash;
  c["metaHash"] = cfg.metaHash;
  c["xpointer"] = cfg.xpointer;
  c["location"] = cfg.location;
  JsonArray progress = c["progress"].to<JsonArray>();
  progress.add(cfg.progressCurrent);
  progress.add(cfg.progressTotal);
  c["updatedAt"] = cfg.updatedAtMs;

  std::string body;
  serializeJson(req, body);

  ReadestHttp::Request rq;
  rq.tag = "RSYNC";
  rq.url = READEST_STORE.getSyncApiBase() + "/sync";
  rq.headers = ReadestHttp::bearerHeaders(accessToken);
  rq.body = &body;
  rq.connectTimeoutMs = SYNC_CONNECT_TIMEOUT;
  rq.readTimeoutMs = SYNC_READ_TIMEOUT;

  JsonDocument doc;
  const int code = ReadestHttp::requestJson(rq, &doc, errMsg);
  if (code == ReadestHttp::JSON_PARSE_FAILED) return JSON_ERROR;
  if (code != 200) return mapHttpStatus(code);

  if (outAuthoritative) {
    for (JsonObjectConst row : doc["configs"].as<JsonArrayConst>()) {
      BookConfig parsed;
      rowToConfig(row, parsed);
      if (parsed.bookHash == cfg.bookHash) {
        *outAuthoritative = std::move(parsed);
        break;
      }
    }
  }
  return OK;
}

const char* ReadestSyncClient::errorString(Error err) {
  switch (err) {
    case OK:
      return "OK";
    case NO_AUTH:
      return "Not signed in";
    case BAD_REQUEST:
      return "Bad request";
    case AUTH_EXPIRED:
      return "Authentication expired";
    case NETWORK_ERROR:
      return "Network error";
    case SERVER_ERROR:
      return "Server error";
    case JSON_ERROR:
      return "Malformed response";
  }
  return "Unknown error";
}
