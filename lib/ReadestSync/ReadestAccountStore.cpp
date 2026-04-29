#include "ReadestAccountStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <ctime>

#include "../../src/JsonSettingsIO.h"

ReadestAccountStore ReadestAccountStore::instance;

namespace {
constexpr char ACCOUNT_FILE_JSON[] = "/.crosspoint/readest.json";

// Hosted-instance defaults (handoff §2.1). Used when the user has not
// overridden them via settings.
constexpr char DEFAULT_SYNC_API_BASE[] = "https://web.readest.com/api";
constexpr char DEFAULT_SUPABASE_URL[] = "https://readest.supabase.co";

// Public Supabase anon key for the hosted instance, base64-encoded (handoff
// §3.2). This is NOT a secret — it's the RLS-gated "anon" key shipped with
// every Readest client. We carry it base64-encoded only because the Lua
// koplugin does, and we want byte-for-byte equivalence with that reference.
//
// Decoded value: a standard HS256 JWT with role=anon issued by Supabase.
constexpr char DEFAULT_ANON_KEY_BASE64[] =
    "ZXlKaGJHY2lPaUpJVXpJMU5pSXNJblI1Y0NJNklrcFhWQ0o5LmV5SnBjM01pT2lKemRYQmhZbUZ6WlNJc0luSmxaaUk2SW5aaWMzbDRablZ6YW1weF"
    "pIaHJhbkZzZVhOaklpd2ljbTlzWlNJNkltRnViMjRpTENKcFlYUWlPakUzTXpReE1qTTJOekVzSW1WNGNDSTZNakEwT1RZNU9UWTNNWDAuM1U1VXFh"
    "b3VfMVNnclZlMWVvOXJBcGMwdUtqcWhwUWRVWGh2d1VIbVVmZw==";

std::string decodeBase64(const std::string& input) {
  if (input.empty()) return {};
  size_t decodedLen = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &decodedLen, reinterpret_cast<const unsigned char*>(input.c_str()),
                                  input.size());
  if (ret != 0 && ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    LOG_ERR("RAS", "base64 decode size query failed (ret=%d)", ret);
    return {};
  }
  std::string out(decodedLen, '\0');
  ret = mbedtls_base64_decode(reinterpret_cast<unsigned char*>(&out[0]), decodedLen, &decodedLen,
                              reinterpret_cast<const unsigned char*>(input.c_str()), input.size());
  if (ret != 0) {
    LOG_ERR("RAS", "base64 decode failed (ret=%d)", ret);
    return {};
  }
  out.resize(decodedLen);
  return out;
}

int64_t nowUnixSeconds() { return static_cast<int64_t>(std::time(nullptr)); }
}  // namespace

bool ReadestAccountStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveReadest(*this, ACCOUNT_FILE_JSON);
}

bool ReadestAccountStore::loadFromFile() {
  if (!Storage.exists(ACCOUNT_FILE_JSON)) {
    LOG_DBG("RAS", "No readest.json — using defaults");
    return false;
  }
  String json = Storage.readFile(ACCOUNT_FILE_JSON);
  if (json.isEmpty()) {
    LOG_ERR("RAS", "readest.json present but empty");
    return false;
  }
  return JsonSettingsIO::loadReadest(*this, json.c_str());
}

std::string ReadestAccountStore::getSyncApiBase() const {
  return syncApiBase.empty() ? std::string(DEFAULT_SYNC_API_BASE) : syncApiBase;
}

std::string ReadestAccountStore::getSupabaseUrl() const {
  return supabaseUrl.empty() ? std::string(DEFAULT_SUPABASE_URL) : supabaseUrl;
}

std::string ReadestAccountStore::getSupabaseAnonKey() const {
  if (!supabaseAnonKey.empty()) {
    return supabaseAnonKey;
  }
  // Decoded once per call — small (~300 B) and called rarely (once per
  // auth request). Caching is not worth the static-init complexity.
  return decodeBase64(DEFAULT_ANON_KEY_BASE64);
}

void ReadestAccountStore::setSyncApiBase(const std::string& url) {
  syncApiBase = url;
  LOG_DBG("RAS", "Sync API base: %s", url.empty() ? "(default)" : url.c_str());
}

void ReadestAccountStore::setSupabaseUrl(const std::string& url) {
  supabaseUrl = url;
  LOG_DBG("RAS", "Supabase URL: %s", url.empty() ? "(default)" : url.c_str());
}

void ReadestAccountStore::setSupabaseAnonKey(const std::string& key) {
  supabaseAnonKey = key;
  LOG_DBG("RAS", "Supabase anon key: %s", key.empty() ? "(default)" : "(custom)");
}

void ReadestAccountStore::setUserEmail(const std::string& email) {
  if (userEmail == email) return;
  userEmail = email;
  LOG_DBG("RAS", "User email set: %s", email.c_str());
  saveToFile();
}

void ReadestAccountStore::setSession(const std::string& email, const std::string& userId,
                                     const std::string& accessToken, const std::string& refreshToken, int64_t expiresAt,
                                     int64_t expiresIn) {
  this->userEmail = email;
  this->userId = userId;
  this->accessToken = accessToken;
  this->refreshToken = refreshToken;
  this->expiresAt = expiresAt;
  this->expiresIn = expiresIn;
  LOG_DBG("RAS", "Session set: user=%s exp=%lld in=%lld", email.c_str(), static_cast<long long>(expiresAt),
          static_cast<long long>(expiresIn));
  saveToFile();
}

void ReadestAccountStore::clearSession() {
  // Note: userEmail and lastConfigsSyncAtMs are intentionally preserved.
  // Email is not a secret — keeping it lets the user re-sign-in with one
  // less keyboard step (sign-in UI requires an email and would reject a
  // post-sign-out attempt otherwise). lastConfigsSyncAtMs is preserved so
  // the same user's pull cursor remains valid; a different user signs in
  // and we get a benign full-re-pull on first sync (different book_hash +
  // meta_hash filter on the server side).
  userId.clear();
  accessToken.clear();
  refreshToken.clear();
  expiresAt = 0;
  expiresIn = 0;
  LOG_DBG("RAS", "Session cleared");
  saveToFile();
}

void ReadestAccountStore::setLastConfigsSyncAtMs(int64_t ms) {
  if (ms == lastConfigsSyncAtMs) return;
  lastConfigsSyncAtMs = ms;
  saveToFile();
}

void ReadestAccountStore::recordSyncResult(bool ok, const std::string& errMsg) {
  if (ok) {
    lastSyncAtMs = nowUnixSeconds() * 1000LL;
    lastSyncError.clear();
  } else {
    lastSyncError = errMsg;
  }
  saveToFile();
}

bool ReadestAccountStore::hasCredentials() const { return !accessToken.empty(); }

bool ReadestAccountStore::needsLogin() const {
  // Per handoff §3.4: token missing OR expires within the next 60 s
  // (treats imminent expiry the same as missing — cheaper than letting
  // the next request fail with 401).
  if (accessToken.empty()) return true;
  return expiresAt < nowUnixSeconds() + 60;
}

bool ReadestAccountStore::needsRefresh() const {
  // Refresh at the half-life (handoff §3.4): once we're past expiresAt -
  // expiresIn/2 we proactively refresh, well before the token expires.
  if (accessToken.empty() || refreshToken.empty()) return false;
  if (expiresIn <= 0) return false;
  return expiresAt < nowUnixSeconds() + (expiresIn / 2);
}
