#pragma once

#include <string>

#include "activities/Activity.h"

/**
 * Sign-in flow for Readest. Mirrors `KOReaderAuthActivity`'s shape but with
 * Readest's actual auth semantics: signIn() establishes the session (it's
 * not a credential validator like kosync's authenticate()), so this activity
 * has to collect the password in addition to running the network call.
 *
 * Flow:
 *   1. Read email from READEST_STORE — must be set in settings first.
 *   2. If a password is saved on the store (set via the settings UI, web or
 *      device), use it. Otherwise prompt via KeyboardEntryActivity.
 *   3. Bring up WiFi if needed.
 *   4. Call ReadestAuthClient::signIn(email, password). Tokens persist on
 *      success; the in-memory copy of the password is scrubbed regardless
 *      of outcome (the persisted copy in the store is left intact).
 *   5. Show outcome and exit on user dismiss.
 */
class ReadestAuthActivity final : public Activity {
 public:
  explicit ReadestAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadestAuth", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == AUTHENTICATING; }

 private:
  enum State {
    NEEDS_EMAIL,      // No email configured — show hint and exit on dismiss
    PASSWORD_PROMPT,  // Awaiting completion of the password keyboard subactivity
    WIFI_SELECTION,   // Awaiting WiFi connection result
    CONNECTING,       // Brief intermediate state
    AUTHENTICATING,   // signIn call in flight
    SUCCESS,
    FAILED,
  };

  State state = PASSWORD_PROMPT;
  std::string statusMessage;
  std::string errorMessage;
  std::string emailCached;      // Snapshot of READEST_STORE.getUserEmail() on entry
  std::string passwordEntered;  // Held only for the duration of the sign-in call

  void launchPasswordPrompt();
  void onPasswordEntered(const std::string& password);
  void onWifiSelectionComplete(bool success);
  void performSignIn();
};
