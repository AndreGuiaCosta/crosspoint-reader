#include "ReadestAccountStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <ctime>

#include "ReadestJsonIO.h"

ReadestAccountStore ReadestAccountStore::instance;

namespace {
constexpr char ACCOUNT_FILE_JSON[] = "/.crosspoint/readest.json";

// Hosted Readest defaults; user overrides via settings (e.g. self-host).
constexpr char DEFAULT_SYNC_API_BASE[] = "https://web.readest.com/api";
constexpr char DEFAULT_SUPABASE_URL[] = "https://readest.supabase.co";
// Public Supabase anon key — RLS-gated "anon" role JWT shipped with every
// Readest client. Not a secret; a self-hoster overrides this with their own
// project's anon key (Supabase dashboard → Project Settings → API).
constexpr char DEFAULT_SUPABASE_ANON_KEY[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InZic3l4ZnVzampxZHhranFseXNjIiwicm9sZSI6ImFub24iLCJpYXQiOjE3MzQxMjM2NzEsImV4cCI6Mj"
    "A0OTY5OTY3MX0.3U5Uqaou_1SgrVe1eo9rApc0uKjqhpQdUXhvwUHmUfg";

int64_t nowUnixSeconds() { return static_cast<int64_t>(std::time(nullptr)); }
}  // namespace

bool ReadestAccountStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return ReadestJsonIO::saveAccount(*this, ACCOUNT_FILE_JSON);
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
  return ReadestJsonIO::loadAccount(*this, json.c_str());
}

std::string ReadestAccountStore::getSyncApiBase() const {
  return syncApiBase.empty() ? std::string(DEFAULT_SYNC_API_BASE) : syncApiBase;
}

std::string ReadestAccountStore::getSupabaseUrl() const {
  return supabaseUrl.empty() ? std::string(DEFAULT_SUPABASE_URL) : supabaseUrl;
}

std::string ReadestAccountStore::getSupabaseAnonKey() const {
  return supabaseAnonKey.empty() ? std::string(DEFAULT_SUPABASE_ANON_KEY) : supabaseAnonKey;
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

void ReadestAccountStore::setPassword(const std::string& pw) {
  if (password == pw) return;
  password = pw;
  LOG_DBG("RAS", "Password %s", pw.empty() ? "cleared" : "set");
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
  // userEmail and lastConfigsSyncAtMs are intentionally preserved so a
  // re-sign-in skips the email keyboard step and reuses the pull cursor.
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
  // Treat imminent (<60 s) expiry the same as missing.
  if (accessToken.empty()) return true;
  return expiresAt < nowUnixSeconds() + 60;
}

bool ReadestAccountStore::needsRefresh() const {
  // Refresh proactively past the half-life.
  if (accessToken.empty() || refreshToken.empty()) return false;
  if (expiresIn <= 0) return false;
  return expiresAt < nowUnixSeconds() + (expiresIn / 2);
}
