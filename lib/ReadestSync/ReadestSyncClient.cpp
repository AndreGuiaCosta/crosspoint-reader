#include "ReadestSyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <ReadestTlsConfig.h>
#include <WiFiClientSecure.h>

#include <cctype>
#include <cstdio>

#include "ReadestAccountStore.h"

namespace {
// Per handoff §4.3 — match the koplugin's budgets. Same TLS handshake +
// JSON parse + DB write on the server, so the wider sync budget is
// reused rather than re-derived.
constexpr int SYNC_CONNECT_TIMEOUT = 5000;
constexpr int SYNC_READ_TIMEOUT = 10000;

// See lib/TestHooks/ReadestTlsConfig.h — production path installs the CA
// bundle; the simulator override calls setInsecure().
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

// Parse `"[17,342]"` (with optional whitespace) into two ints. Tolerates
// the variations the server may emit; gracefully returns false on
// anything else.
bool parseProgressString(const std::string& s, int* cur, int* total) {
  if (s.empty()) return false;
  return std::sscanf(s.c_str(), " [ %d , %d ]", cur, total) == 2;
}

// ISO-8601 → unix ms. Accepts `YYYY-MM-DDTHH:MM:SS[.fff]Z` and trailing
// timezone variants by treating anything after the seconds (or fractional
// seconds) as UTC. Uses Howard Hinnant's days_from_civil so we don't
// depend on platform timegm() — newlib on ESP-IDF has it, glibc has it,
// but neither guarantee is worth a future debugging session.
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
  // days_from_civil — see https://howardhinnant.github.io/date_algorithms.html
  Y -= M <= 2;
  const int era = (Y >= 0 ? Y : Y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(Y - era * 400);
  const unsigned doy = (153u * (M > 2 ? M - 3 : M + 9) + 2u) / 5u + D - 1;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  return days * 86400000LL + static_cast<int64_t>(h) * 3600000LL + static_cast<int64_t>(m) * 60000LL +
         static_cast<int64_t>(s) * 1000LL + ms;
}

// Populate a BookConfig from one snake_case row of the GET response.
// `progress` is the stringified-array quirk (§13.5); `updated_at` is the
// ISO-string quirk (§13.7).
void rowToConfig(JsonObjectConst row, ReadestSyncClient::BookConfig& out) {
  out.bookHash = row["book_hash"] | std::string("");
  out.metaHash = row["meta_hash"] | std::string("");
  out.xpointer = row["xpointer"] | std::string("");
  out.location = row["location"] | std::string("");

  const std::string progressStr = row["progress"] | std::string("");
  parseProgressString(progressStr, &out.progressCurrent, &out.progressTotal);

  const std::string updatedAt = row["updated_at"] | std::string("");
  out.updatedAtMs = parseIso8601ToMs(updatedAt);

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

  // Hashes are 32-hex; `since` is numeric. Nothing here needs URL-encoding,
  // and skipping a generic encoder keeps the firmware binary small.
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

  // Per §4.1 the server's filter unions book and meta_hash matches, so the
  // result may include rows for other books with the same metadata. Pick
  // the row whose book_hash matches ours; ignore the rest. Also track the
  // max updated_at across *all* returned rows so the cursor advances even
  // when the matching row is older than a sibling.
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

  // Build { configs: [{ ... }] }. Field names are camelCase per §6.1 —
  // server's transform layer maps them to snake_case columns. `progress`
  // goes out as an array (§13.5); the response will contain the
  // stringified form.
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

  // The server returns `{configs: [authoritative_row]}`. Match by hash —
  // belt and suspenders since we only sent one — and adopt the row.
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
