#include "ReadestSyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <ReadestTlsConfig.h>
#include <WiFiClientSecure.h>

#include <cstdio>

#include "ReadestAccountStore.h"
#include "ReadestTimeUtils.h"

namespace {
constexpr int SYNC_CONNECT_TIMEOUT = 5000;
constexpr int SYNC_READ_TIMEOUT = 10000;

void configureTls(WiFiClientSecure& client) { ReadestTls::configure(client); }

void addAuthHeaders(HTTPClient& http, const std::string& accessToken) {
  http.addHeader("Authorization", (std::string("Bearer ") + accessToken).c_str());
  http.addHeader("Accept", "application/json");
}

ReadestSyncClient::Error mapHttpStatus(int code) {
  using E = ReadestSyncClient::Error;
  if (code == 200) return E::OK;
  if (code == 400) return E::BAD_REQUEST;
  if (code == 401 || code == 403) return E::AUTH_EXPIRED;
  if (code >= 500) return E::SERVER_ERROR;
  if (code < 0) return E::NETWORK_ERROR;
  return E::SERVER_ERROR;
}

void extractErrorMessage(const String& body, std::string* errMsg) {
  if (!errMsg) return;
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length()) != DeserializationError::Ok) return;
  std::string msg = doc["error"] | std::string("");
  if (msg.empty()) msg = doc["message"] | std::string("");
  if (!msg.empty()) *errMsg = std::move(msg);
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
  std::string url = READEST_STORE.getSyncApiBase() + "/sync?since=" + sinceBuf + "&type=configs&book=" + bookHash +
                    "&meta_hash=" + metaHash;
  LOG_DBG("RSYNC", "pull: %s", url.c_str());

  WiFiClientSecure secureClient;
  configureTls(secureClient);

  HTTPClient http;
  http.setConnectTimeout(SYNC_CONNECT_TIMEOUT);
  http.setTimeout(SYNC_READ_TIMEOUT);
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http, accessToken);

  const int code = http.GET();
  const String response = http.getString();
  http.end();
  LOG_DBG("RSYNC", "pull HTTP %d body=%u bytes", code, response.length());

  if (code != 200) {
    extractErrorMessage(response, errMsg);
    return mapHttpStatus(code);
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, response.c_str(), response.length());
  if (err) {
    LOG_ERR("RSYNC", "pull JSON parse: %s", err.c_str());
    return JSON_ERROR;
  }

  // Server filter unions book and meta_hash matches, so the response may
  // include same-meta rows for other books — pick the matching book_hash,
  // and track maxUpdatedAtMs across *all* rows so the cursor advances even
  // when the match is older than a sibling row.
  JsonArrayConst configs = doc["configs"];
  for (JsonObjectConst row : configs) {
    BookConfig parsed;
    rowToConfig(row, parsed);
    if (maxUpdatedAtMs && parsed.updatedAtMs > *maxUpdatedAtMs) {
      *maxUpdatedAtMs = parsed.updatedAtMs;
    }
    if (out && out->bookHash.empty() && parsed.bookHash == bookHash) {
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

  const std::string url = READEST_STORE.getSyncApiBase() + "/sync";
  LOG_DBG("RSYNC", "push: %s", url.c_str());

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

  WiFiClientSecure secureClient;
  configureTls(secureClient);

  HTTPClient http;
  http.setConnectTimeout(SYNC_CONNECT_TIMEOUT);
  http.setTimeout(SYNC_READ_TIMEOUT);
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http, accessToken);
  http.addHeader("Content-Type", "application/json");

  const int code = http.POST(body.c_str());
  const String response = http.getString();
  http.end();
  LOG_DBG("RSYNC", "push HTTP %d body=%u bytes", code, response.length());

  if (code != 200) {
    extractErrorMessage(response, errMsg);
    return mapHttpStatus(code);
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, response.c_str(), response.length());
  if (err) {
    LOG_ERR("RSYNC", "push JSON parse: %s", err.c_str());
    return JSON_ERROR;
  }

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
