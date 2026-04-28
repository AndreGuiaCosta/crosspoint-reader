#include "ReadestStorageClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <ReadestTlsConfig.h>
#include <WiFiClientSecure.h>

#include <cctype>

#include "ReadestAccountStore.h"

namespace {
// Match the budgets used by ReadestSyncClient (handoff §4.3). The storage
// list/download endpoints share the same Next.js handlers and DB roundtrip
// shape as /sync, so the same window applies.
constexpr int STORAGE_CONNECT_TIMEOUT = 5000;
constexpr int STORAGE_READ_TIMEOUT = 10000;

// All-zero book_hash sentinel from the server's "empty books" hotfix
// (handoff §13.1). Filter at the client so activities never see it.
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

// ISO-8601 → unix ms. Same algorithm as ReadestSyncClient::parseIso8601ToMs;
// duplicated here to avoid coupling the two clients via a shared internal
// header. Howard Hinnant's days_from_civil so we don't depend on platform
// timegm() — newlib has it, glibc has it, neither guarantee is worth a
// future debugging session.
int64_t parseIso8601ToMs(const std::string& iso) {
  int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return 0;
  int ms = 0;
  const size_t dot = iso.find('.', 19);
  if (dot != std::string::npos) {
    int frac = 0, digits = 0;
    for (size_t i = dot + 1; i < iso.size() && std::isdigit(static_cast<unsigned char>(iso[i])) && digits < 3;
         ++i, ++digits) {
      frac = frac * 10 + (iso[i] - '0');
    }
    while (digits < 3) {
      frac *= 10;
      ++digits;
    }
    ms = frac;
  }
  Y -= M <= 2;
  const int era = (Y >= 0 ? Y : Y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(Y - era * 400);
  const unsigned doy = (153u * (M > 2 ? M - 3 : M + 9) + 2u) / 5u + D - 1;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  return days * 86400000LL + static_cast<int64_t>(h) * 3600000LL + static_cast<int64_t>(m) * 60000LL +
         static_cast<int64_t>(s) * 1000LL + ms;
}

// books table response is snake_case (server-side column names). `progress`
// is a JSON array on books (NOT stringified — that quirk is configs-only,
// handoff §13.5).
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
  out.uploadedAtMs = parseIso8601ToMs(uploadedAt);
  const std::string updatedAt = row["updated_at"] | std::string("");
  out.updatedAtMs = parseIso8601ToMs(updatedAt);
  const std::string deletedAt = row["deleted_at"] | std::string("");
  out.deleted = !deletedAt.empty();
}

void rowToFile(JsonObjectConst row, ReadestStorageClient::FileRow& out) {
  out.fileKey = row["file_key"] | std::string("");
  out.fileSize = row["file_size"] | static_cast<int64_t>(0);
  out.bookHash = row["book_hash"] | std::string("");
  const std::string updatedAt = row["updated_at"] | std::string("");
  out.updatedAtMs = parseIso8601ToMs(updatedAt);
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
    if (parsed.hash == DUMMY_BOOK_HASH) continue;  // handoff §13.1
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

  // bookHash is 32-hex; no encoding needed.
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

  // Body shape: { "fileKeys": ["...","..."] } — handoff §14.4.2.
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
  // Response: { "downloadUrls": { "<key>": "<signed-url>", ... } }. Keys
  // the server couldn't resolve are simply absent from the map.
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
