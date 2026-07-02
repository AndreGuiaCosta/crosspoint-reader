#include "ReadestStorageClient.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cstdio>

#include "ReadestAccountStore.h"
#include "ReadestHttp.h"
#include "ReadestTimeUtils.h"

namespace {
constexpr int STORAGE_CONNECT_TIMEOUT = 5000;
constexpr int STORAGE_READ_TIMEOUT = 15000;

constexpr char DUMMY_BOOK_HASH[] = "00000000000000000000000000000000";

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
  const std::string syncedAt = row["synced_at"] | std::string("");
  out.syncedAtMs = ReadestTimeUtils::parseIso8601ToMs(syncedAt);
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

  ReadestHttp::Request rq;
  rq.tag = "RSTOR";
  rq.url = READEST_STORE.getSyncApiBase() + "/sync?since=" + sinceBuf + "&type=books";
  rq.headers = ReadestHttp::bearerHeaders(accessToken);
  rq.connectTimeoutMs = STORAGE_CONNECT_TIMEOUT;
  rq.readTimeoutMs = STORAGE_READ_TIMEOUT;

  // A first pull (since=0) returns the entire library in one body; the filter
  // bounds the parsed document to the fields rowToBook reads.
  JsonDocument filter;
  JsonObject f = filter["books"].add<JsonObject>();
  f["book_hash"] = true;
  f["meta_hash"] = true;
  f["format"] = true;
  f["title"] = true;
  f["source_title"] = true;
  f["author"] = true;
  f["progress"] = true;
  f["uploaded_at"] = true;
  f["updated_at"] = true;
  f["synced_at"] = true;
  f["deleted_at"] = true;
  rq.filter = &filter;

  JsonDocument doc;
  const int code = ReadestHttp::requestJson(rq, &doc, errMsg);
  if (code == ReadestHttp::JSON_PARSE_FAILED) return JSON_ERROR;
  if (code != 200) return mapHttpStatus(code);

  if (out) out->reserve(doc["books"].as<JsonArrayConst>().size());
  for (JsonObjectConst row : doc["books"].as<JsonArrayConst>()) {
    BookRow parsed;
    rowToBook(row, parsed);
    if (parsed.hash == DUMMY_BOOK_HASH) continue;
    // Cursor from synced_at (the column the server filters on); fall back to
    // updated_at against pre-synced_at servers, where the two are equivalent.
    const int64_t cursorMs = parsed.syncedAtMs > 0 ? parsed.syncedAtMs : parsed.updatedAtMs;
    if (maxUpdatedAtMs && cursorMs > *maxUpdatedAtMs) {
      *maxUpdatedAtMs = cursorMs;
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

  ReadestHttp::Request rq;
  rq.tag = "RSTOR";
  rq.url = READEST_STORE.getSyncApiBase() + "/storage/list?bookHash=" + bookHash;
  rq.headers = ReadestHttp::bearerHeaders(accessToken);
  rq.connectTimeoutMs = STORAGE_CONNECT_TIMEOUT;
  rq.readTimeoutMs = STORAGE_READ_TIMEOUT;

  JsonDocument doc;
  const int code = ReadestHttp::requestJson(rq, &doc, errMsg);
  if (code == ReadestHttp::JSON_PARSE_FAILED) return JSON_ERROR;
  if (code != 200) return mapHttpStatus(code);

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

  JsonDocument req;
  JsonArray arr = req["fileKeys"].to<JsonArray>();
  for (const auto& key : fileKeys) arr.add(key);
  std::string body;
  serializeJson(req, body);

  ReadestHttp::Request rq;
  rq.tag = "RSTOR";
  rq.url = READEST_STORE.getSyncApiBase() + "/storage/download";
  rq.headers = ReadestHttp::bearerHeaders(accessToken);
  rq.body = &body;
  rq.connectTimeoutMs = STORAGE_CONNECT_TIMEOUT;
  rq.readTimeoutMs = STORAGE_READ_TIMEOUT;

  JsonDocument doc;
  const int code = ReadestHttp::requestJson(rq, &doc, errMsg);
  if (code == ReadestHttp::JSON_PARSE_FAILED) return JSON_ERROR;
  if (code != 200) return mapHttpStatus(code);

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
