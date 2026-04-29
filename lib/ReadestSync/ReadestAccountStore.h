#pragma once
#include <cstdint>
#include <string>

class ReadestAccountStore;
namespace JsonSettingsIO {
bool saveReadest(const ReadestAccountStore& store, const char* path);
bool loadReadest(ReadestAccountStore& store, const char* json);
}  // namespace JsonSettingsIO

/**
 * Single-account credential + endpoint store for Readest Sync.
 *
 * Holds the configurable URLs (sync API base, Supabase auth base, anon
 * key — overridable for self-hosted instances) and the per-session
 * tokens issued by Supabase GoTrue (access, refresh, expiry). Persists
 * as plain JSON to /.crosspoint/readest.json. Tokens are NOT obfuscated
 * in V1 (decision documented in CROSSPOINT_SYNC_HANDOFF.md handoff
 * sign-off): they only protect access to one user's reading progress
 * on one device's SD, and Supabase rotates them every hour.
 *
 * The default Supabase anon key is published openly in Readest's
 * koplugin (handoff §3.2) and ships with this binary base64-encoded;
 * it is decoded on first access if no override has been set.
 */
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

  // Session tokens.
  std::string accessToken;
  std::string refreshToken;
  int64_t expiresAt = 0;  // Unix seconds (Supabase wire format).
  int64_t expiresIn = 0;  // Seconds; used to compute the refresh half-life.

  // Last `since` cursor used for /api/sync pulls. Unix milliseconds.
  int64_t lastConfigsSyncAtMs = 0;

  // Status of the most recent progress sync attempt (handoff §17). Updated
  // by ReadestSyncCoordinator on every pull/push. lastSyncAtMs is set to
  // wall-clock-ms on success and left untouched on failure; lastSyncError
  // holds the most recent error message and is cleared on success.
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
  // self-hosted users override these via settings (Phase 5/6 UI).
  const std::string& getSyncApiBaseRaw() const { return syncApiBase; }
  const std::string& getSupabaseUrlRaw() const { return supabaseUrl; }
  const std::string& getSupabaseAnonKeyRaw() const { return supabaseAnonKey; }

  std::string getSyncApiBase() const;
  std::string getSupabaseUrl() const;
  // Returns the anon key to send as the `apikey` header. Falls back to the
  // base64-encoded blob shipped with the binary, decoded once on demand.
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

  // Settings UI lets the user pre-set their email before signing in, so the
  // sign-in activity can use it without prompting twice. Persists to disk.
  // Token state is left untouched.
  void setUserEmail(const std::string& email);

  // Replace the active session with a fresh token bundle (post sign-in or
  // refresh). Persists to disk.
  void setSession(const std::string& email, const std::string& userId, const std::string& accessToken,
                  const std::string& refreshToken, int64_t expiresAt, int64_t expiresIn);
  // Wipe identity + tokens (sign-out or invalidation). Persists to disk.
  void clearSession();

  int64_t getLastConfigsSyncAtMs() const { return lastConfigsSyncAtMs; }
  void setLastConfigsSyncAtMs(int64_t ms);

  int64_t getLastSyncAtMs() const { return lastSyncAtMs; }
  const std::string& getLastSyncError() const { return lastSyncError; }
  // Record the outcome of a sync attempt. On success: stamp lastSyncAtMs
  // with wall-clock-ms and clear lastSyncError. On failure: leave the
  // timestamp alone (so the user still sees when it last worked) and
  // record errMsg. Persists.
  void recordSyncResult(bool ok, const std::string& errMsg);

  // Predicates.
  // True iff a non-empty access token is present. Does not check expiry —
  // use needsLogin() for that.
  bool hasCredentials() const;
  // True iff sign-in is required: token missing OR expires within the next
  // 60 s. Caller should prompt for credentials.
  bool needsLogin() const;
  // True iff the token should be refreshed proactively: past the half-life
  // (expires_at < now + expires_in/2). Cheaper than waiting for it to expire.
  //
  // Note: Readest's hosted Supabase issues 7-day access tokens (expires_in
  // = 604800), not the typical 3600. With that horizon, lazy refresh —
  // letting a sync call return AUTH_EXPIRED on 401/403 and retrying once
  // after refresh — is usually preferable to calling this on every sync.
  // Reserve needsRefresh for boot-time prewarming or long idle resumes.
  bool needsRefresh() const;
};

#define READEST_STORE ReadestAccountStore::getInstance()
