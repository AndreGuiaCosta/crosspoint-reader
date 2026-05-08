#pragma once
#include <cstdint>
#include <string>

class ReadestAccountStore;
namespace JsonSettingsIO {
bool saveReadest(const ReadestAccountStore& store, const char* path);
bool loadReadest(ReadestAccountStore& store, const char* json);
}  // namespace JsonSettingsIO

// Single-account credential + endpoint store for Readest Sync. Persists
// as plain JSON to /.crosspoint/readest.json.
class ReadestAccountStore {
 private:
  static ReadestAccountStore instance;

  // Endpoints (URLs without trailing slash; auth client appends paths).
  std::string syncApiBase;      // e.g. "https://web.readest.com/api"
  std::string supabaseUrl;      // e.g. "https://readest.supabase.co"
  std::string supabaseAnonKey;  // The anon JWT, NOT the user access token.

  // Identity (populated after a successful sign-in).
  std::string userEmail;
  std::string userId;

  // Persisted XOR-obfuscated. Only ever set by the user (settings UI) —
  // never returned by /api/settings GET.
  std::string password;

  // Session tokens.
  std::string accessToken;
  std::string refreshToken;
  int64_t expiresAt = 0;  // Unix seconds (Supabase wire format).
  int64_t expiresIn = 0;  // Seconds; used to compute the refresh half-life.

  // Last `since` cursor used for /api/sync pulls. Unix milliseconds.
  int64_t lastConfigsSyncAtMs = 0;

  // Outcome of the most recent sync attempt. lastSyncAtMs is stamped on
  // success and left untouched on failure.
  int64_t lastSyncAtMs = 0;
  std::string lastSyncError;

  ReadestAccountStore() = default;

  friend bool JsonSettingsIO::saveReadest(const ReadestAccountStore&, const char*);
  friend bool JsonSettingsIO::loadReadest(ReadestAccountStore&, const char*);

 public:
  ReadestAccountStore(const ReadestAccountStore&) = delete;
  ReadestAccountStore& operator=(const ReadestAccountStore&) = delete;

  static ReadestAccountStore& getInstance() { return instance; }

  // Persistence.
  bool saveToFile() const;
  bool loadFromFile();

  // Endpoint accessors. Empty values are treated as "use the hosted default";
  // self-hosted users override these via settings.
  const std::string& getSyncApiBaseRaw() const { return syncApiBase; }
  const std::string& getSupabaseUrlRaw() const { return supabaseUrl; }
  const std::string& getSupabaseAnonKeyRaw() const { return supabaseAnonKey; }

  std::string getSyncApiBase() const;
  std::string getSupabaseUrl() const;
  // Anon key for the `apikey` header; decodes the shipped default if unset.
  std::string getSupabaseAnonKey() const;

  void setSyncApiBase(const std::string& url);
  void setSupabaseUrl(const std::string& url);
  void setSupabaseAnonKey(const std::string& key);

  // Identity + session.
  const std::string& getUserEmail() const { return userEmail; }
  const std::string& getUserId() const { return userId; }
  const std::string& getAccessToken() const { return accessToken; }
  const std::string& getRefreshToken() const { return refreshToken; }
  int64_t getExpiresAt() const { return expiresAt; }
  int64_t getExpiresIn() const { return expiresIn; }

  // Persists email; token state is left untouched.
  void setUserEmail(const std::string& email);

  // Empty string means "no password saved — prompt on sign-in".
  const std::string& getPassword() const { return password; }
  void setPassword(const std::string& pw);

  // Replace the active session with a fresh token bundle. Persists.
  void setSession(const std::string& email, const std::string& userId, const std::string& accessToken,
                  const std::string& refreshToken, int64_t expiresAt, int64_t expiresIn);
  // Wipe identity + tokens. Persists.
  void clearSession();

  int64_t getLastConfigsSyncAtMs() const { return lastConfigsSyncAtMs; }
  void setLastConfigsSyncAtMs(int64_t ms);

  int64_t getLastSyncAtMs() const { return lastSyncAtMs; }
  const std::string& getLastSyncError() const { return lastSyncError; }
  // On success stamps lastSyncAtMs and clears lastSyncError; on failure
  // leaves the timestamp and records errMsg. Persists.
  void recordSyncResult(bool ok, const std::string& errMsg);

  // True iff a non-empty access token is present (does not check expiry).
  bool hasCredentials() const;
  // True iff sign-in is required: token missing OR expires within 60 s.
  bool needsLogin() const;
  // True iff past the token half-life. Hosted Supabase issues 7-day tokens,
  // so lazy refresh on AUTH_EXPIRED is usually preferable to checking this
  // on every call; reserve for boot prewarming or long idle resumes.
  bool needsRefresh() const;
};

#define READEST_STORE ReadestAccountStore::getInstance()
