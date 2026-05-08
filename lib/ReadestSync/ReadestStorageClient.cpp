#include "ReadestStorageClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <ReadestTlsConfig.h>
#include <WiFiClientSecure.h>

#include "ReadestAccountStore.h"
#include "ReadestTimeUtils.h"

namespace {
constexpr int STORAGE_CONNECT_TIMEOUT = 5000;
constexpr int STORAGE_READ_TIMEOUT = 10000;

constexpr char DUMMY_BOOK_HASH[] = "00000000000000000000000000000000";

void configureTls(WiFiClientSecure& client) { ReadestTls::configure(client); }

void addAuthHeaders(HTTPClient& http, const std::string& accessToken) {
  http.addHeader("Authorization", (std::string("Bearer ") + accessToken).c_str());
  http.addHeader("Accept", "application/json");
}

ReadestStorageClient::Error mapHttpStatus(int code) {
  using E = ReadestStorageClient::Error;
  if (code == 200) return E::OK;
  if (code == 400) return E::BAD_REQUEST;
  if (code == 401 || code == 403) return E::AUTH_EXPIRED;
  if (code == 404) return E::NOT_FOUND;
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

void rowToBook(JsonObjectConst row, ReadestStorageClient::BookRow& out) {
  out.hash = row["book_hash"] | std::string("");
  out.metaHash = row["meta_hash"] | std::string("");
  out.format = row["format"] | std::string("");
  out.title = row["title"] | std::string("");
  out.sourceTitle = row["source_title"] | std::string("");
  out.author = row["author"] | std::string("");

  JsonArrayConst progress = row["progress"];
  if (!progress.isNull() && progress.size() >= 2) {
    out.progressCurrent = progress[0] | 0;
    out.progressTotal = progress[1] | 0;
  }

  const std::string uploadedAt = row["uploaded_at"] | std::string("");
  out.uploadedAtMs = ReadestTimeUtils::parseIso8601ToMs(uploadedAt);
  const std::string updatedAt = row["updated_at"] | std::string("");
  out.updatedAtMs = ReadestTimeUtils::parseIso8601ToMs(updatedAt);
  const std::string deletedAt = row["deleted_at"] | std::string("");
  out.deleted = !deletedAt.empty();
}

void rowToFile(JsonObjectConst row, ReadestStorageClient::FileRow& out) {
  out.fileKey = row["file_key"] | std::string("");
  out.fileSize = row["file_size"] | static_cast<int64_t>(0);
  out.bookHash = row["book_hash"] | std::string("");
  const std::string updatedAt = row["updated_at"] | std::string("");
  out.updatedAtMs = ReadestTimeUtils::parseIso8601ToMs(updatedAt);
}
}  // namespace

ReadestStorageClient::Error ReadestStorageClient::pullBooksSince(int64_t sinceMs, std::vector<BookRow>* out,
                                                                 int64_t* maxUpdatedAtMs, std::string* errMsg) {
  if (out) out->clear();
  if (maxUpdatedAtMs) *maxUpdatedAtMs = 0;

  const std::string accessToken = READEST_STORE.getAccessToken();
  if (accessToken.empty()) {
    LOG_DBG("RSTOR", "books pull: no access token");
    return NO_AUTH;
  }

  char sinceBuf[32];
  std::snprintf(sinceBuf, sizeof(sinceBuf), "%lld", static_cast<long long>(sinceMs));
  const std::string url = READEST_STORE.getSyncApiBase() + "/sync?since=" + sinceBuf + "&type=books";
  LOG_DBG("RSTOR", "books pull: %s", url.c_str());

  WiFiClientSecure secureClient;
  configureTls(secureClient);

  HTTPClient http;
  http.setConnectTimeout(STORAGE_CONNECT_TIMEOUT);
  http.setTimeout(STORAGE_READ_TIMEOUT);
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http, accessToken);

  const int code = http.GET();
  const String response = http.getString();
  http.end();
  LOG_DBG("RSTOR", "books pull HTTP %d body=%u bytes", code, response.length());

  if (code != 200) {
    extractErrorMessage(response, errMsg);
    return mapHttpStatus(code);
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, response.c_str(), response.length());
  if (err) {
    LOG_ERR("RSTOR", "books pull JSON parse: %s", err.c_str());
    return JSON_ERROR;
  }

  if (out) out->reserve(doc["books"].as<JsonArrayConst>().size());
  for (JsonObjectConst row : doc["books"].as<JsonArrayConst>()) {
    BookRow parsed;
    rowToBook(row, parsed);
    if (parsed.hash == DUMMY_BOOK_HASH) continue;
    if (maxUpdatedAtMs && parsed.updatedAtMs > *maxUpdatedAtMs) {
      *maxUpdatedAtMs = parsed.updatedAtMs;
    }
    if (out) out->push_back(std::move(parsed));
  }
  return OK;
}

ReadestStorageClient::Error ReadestStorageClient::listFilesByBookHash(const std::string& bookHash,
                                                                      std::vector<FileRow>* out, std::string* errMsg) {
  if (out) out->clear();

  const std::string accessToken = READEST_STORE.getAccessToken();
  if (accessToken.empty()) {
    LOG_DBG("RSTOR", "list: no access token");
    return NO_AUTH;
  }

  const std::string url = READEST_STORE.getSyncApiBase() + "/storage/list?bookHash=" + bookHash;
  LOG_DBG("RSTOR", "list: %s", url.c_str());

  WiFiClientSecure secureClient;
  configureTls(secureClient);

  HTTPClient http;
  http.setConnectTimeout(STORAGE_CONNECT_TIMEOUT);
  http.setTimeout(STORAGE_READ_TIMEOUT);
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http, accessToken);

  const int code = http.GET();
  const String response = http.getString();
  http.end();
  LOG_DBG("RSTOR", "list HTTP %d body=%u bytes", code, response.length());

  if (code != 200) {
    extractErrorMessage(response, errMsg);
    return mapHttpStatus(code);
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, response.c_str(), response.length());
  if (err) {
    LOG_ERR("RSTOR", "list JSON parse: %s", err.c_str());
    return JSON_ERROR;
  }

  if (out) out->reserve(doc["files"].as<JsonArrayConst>().size());
  for (JsonObjectConst row : doc["files"].as<JsonArrayConst>()) {
    FileRow parsed;
    rowToFile(row, parsed);
    if (out) out->push_back(std::move(parsed));
  }
  return OK;
}

ReadestStorageClient::Error ReadestStorageClient::getDownloadUrls(const std::vector<std::string>& fileKeys,
                                                                  std::map<std::string, std::string>* outUrls,
                                                                  std::string* errMsg) {
  if (outUrls) outUrls->clear();
  if (fileKeys.empty()) return OK;

  const std::string accessToken = READEST_STORE.getAccessToken();
  if (accessToken.empty()) {
    LOG_DBG("RSTOR", "sign: no access token");
    return NO_AUTH;
  }

  const std::string url = READEST_STORE.getSyncApiBase() + "/storage/download";
  LOG_DBG("RSTOR", "sign: %s (%u keys)", url.c_str(), static_cast<unsigned>(fileKeys.size()));

  JsonDocument req;
  JsonArray arr = req["fileKeys"].to<JsonArray>();
  for (const auto& key : fileKeys) arr.add(key);
  std::string body;
  serializeJson(req, body);

  WiFiClientSecure secureClient;
  configureTls(secureClient);

  HTTPClient http;
  http.setConnectTimeout(STORAGE_CONNECT_TIMEOUT);
  http.setTimeout(STORAGE_READ_TIMEOUT);
  http.begin(secureClient, url.c_str());
  addAuthHeaders(http, accessToken);
  http.addHeader("Content-Type", "application/json");

  const int code = http.POST(body.c_str());
  const String response = http.getString();
  http.end();
  LOG_DBG("RSTOR", "sign HTTP %d body=%u bytes", code, response.length());

  if (code != 200) {
    extractErrorMessage(response, errMsg);
    return mapHttpStatus(code);
  }

  JsonDocument doc;
  const auto err = deserializeJson(doc, response.c_str(), response.length());
  if (err) {
    LOG_ERR("RSTOR", "sign JSON parse: %s", err.c_str());
    return JSON_ERROR;
  }

  if (!outUrls) return OK;
  JsonObjectConst urls = doc["downloadUrls"].as<JsonObjectConst>();
  for (JsonPairConst kv : urls) {
    const char* signed_url = kv.value().as<const char*>();
    if (signed_url == nullptr) continue;
    (*outUrls)[std::string(kv.key().c_str())] = std::string(signed_url);
  }
  return OK;
}

const char* ReadestStorageClient::errorString(Error err) {
  switch (err) {
    case OK:
      return "OK";
    case NO_AUTH:
      return "Not signed in";
    case BAD_REQUEST:
      return "Bad request";
    case AUTH_EXPIRED:
      return "Authentication expired";
    case NOT_FOUND:
      return "Not found";
    case NETWORK_ERROR:
      return "Network error";
    case SERVER_ERROR:
      return "Server error";
    case JSON_ERROR:
      return "Malformed response";
  }
  return "Unknown error";
}
