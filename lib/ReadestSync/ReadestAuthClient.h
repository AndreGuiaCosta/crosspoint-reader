#pragma once
#include <string>

// Supabase GoTrue auth client. signIn / refresh write the session into
// READEST_STORE; signOut clears it. Time-of-day must be NTP-synced first —
// Supabase verifies token timestamps and a skewed clock loops on refresh.
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

  // Authenticate with email + password. errMsg receives the server's
  // error_description / msg field on 4xx if non-null.
  static Error signIn(const std::string& email, const std::string& password, std::string* errMsg = nullptr);

  // Refresh the access token using the stored refresh_token.
  static Error refresh(std::string* errMsg = nullptr);

  // Best-effort sign-out: clears the local session regardless of HTTP outcome.
  static Error signOut();

  static const char* errorString(Error err);
};
