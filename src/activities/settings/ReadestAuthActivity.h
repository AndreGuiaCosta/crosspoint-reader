#pragma once

#include <string>

#include "activities/Activity.h"

// Sign-in flow for Readest. Reads email from READEST_STORE (must be set
// first), uses a saved password or prompts via KeyboardEntryActivity, brings
// up WiFi, then calls ReadestAuthClient::signIn. The in-memory password copy
// is scrubbed regardless of outcome.
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
