#pragma once
#include <string>

/**
 * Supabase GoTrue auth client for Readest Sync. Implements the three
 * password-grant flows used by the Readest backend (see handoff §3):
 *
 *   POST {supabase}/auth/v1/token?grant_type=password
 *   POST {supabase}/auth/v1/token?grant_type=refresh_token
 *   POST {supabase}/auth/v1/logout
 *
 * On success, signIn / refresh write the resulting session into
 * ReadestAccountStore (READEST_STORE). signOut clears it.
 *
 * Time-of-day must be NTP-synced before calling these — Supabase
 * verifies token timestamps and a skewed clock will loop on refresh.
 */
class ReadestAuthClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,       // signIn called with empty fields, or refresh with no refresh_token
    NO_ANON_KEY,          // anon key not configured (and default decode failed)
    NETWORK_ERROR,        // HTTPClient returned negative status
    INVALID_CREDENTIALS,  // 400 / 401 — bad email/password or expired refresh token
    FORBIDDEN,            // 403 — auth required but missing
    SERVER_ERROR,         // 5xx
    JSON_ERROR,           // response parse failure
  };

  /**
   * Authenticate with email and password. On success, populates the
   * session in READEST_STORE and persists to disk.
   *
   * @param email user-provided email
   * @param password user-provided password (cleartext, sent over TLS)
   * @param errMsg optional output for the server's error_description /
   *               msg field on 4xx; useful for surfacing in UI.
   */
  static Error signIn(const std::string& email, const std::string& password, std::string* errMsg = nullptr);

  /**
   * Refresh the access token using the stored refresh_token. On success,
   * updates the session in READEST_STORE and persists.
   */
  static Error refresh(std::string* errMsg = nullptr);

  /**
   * Sign out. Best-effort: tells Supabase to invalidate the token, then
   * clears the local session regardless of HTTP outcome (so the user
   * isn't stuck if the server is unreachable).
   */
  static Error signOut();

  static const char* errorString(Error err);
};
