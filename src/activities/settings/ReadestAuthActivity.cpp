#include "ReadestAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <NtpSync.h>
#include <ReadestAccountStore.h>
#include <ReadestAuthClient.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ReadestAuthActivity::launchPasswordPrompt() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READEST_PASSWORD_PROMPT),
                                                                 "", 128, InputType::Password),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             finish();
                             return;
                           }
                           const auto& kb = std::get<KeyboardResult>(result.data);
                           onPasswordEntered(kb.text);
                         });
}

void ReadestAuthActivity::onPasswordEntered(const std::string& password) {
  if (password.empty()) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = "Password is required";
    requestUpdate();
    return;
  }

  passwordEntered = password;

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  {
    RenderLock lock(*this);
    state = WIFI_SELECTION;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadestAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    RenderLock lock(*this);
    state = FAILED;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = tr(STR_AUTHENTICATING);
  }
  requestUpdate();

  // Supabase rejects skewed clocks on token issuance — match the sync
  // activity's NTP precondition.
  NtpSync::syncTime();

  performSignIn();
}

void ReadestAuthActivity::performSignIn() {
  std::string errMsg;
  const auto rc = ReadestAuthClient::signIn(emailCached, passwordEntered, &errMsg);
  // Drop password from memory regardless of outcome.
  passwordEntered.clear();

  RenderLock lock(*this);
  if (rc == ReadestAuthClient::OK) {
    state = SUCCESS;
    statusMessage = tr(STR_AUTH_SUCCESS);
  } else {
    state = FAILED;
    errorMessage = errMsg.empty() ? std::string(ReadestAuthClient::errorString(rc))
                                  : (std::string(ReadestAuthClient::errorString(rc)) + " (" + errMsg + ")");
  }
  requestUpdate();
}

void ReadestAuthActivity::onEnter() {
  Activity::onEnter();

  emailCached = READEST_STORE.getUserEmail();
  if (emailCached.empty()) {
    state = NEEDS_EMAIL;
    requestUpdate();
    return;
  }

  state = PASSWORD_PROMPT;
  launchPasswordPrompt();
}

void ReadestAuthActivity::onExit() {
  Activity::onExit();

  // Belt-and-suspenders password scrub. onEnter clears it before signIn,
  // and signIn's destructor would too, but exiting from any other path
  // (cancel, error mid-flow) shouldn't leave it in memory.
  passwordEntered.clear();

  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

void ReadestAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READEST_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == NEEDS_EMAIL) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_READEST_NEEDS_EMAIL), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_READEST_NEEDS_EMAIL_HINT));
  } else if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_SUCCESS), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_READEST_SYNC_READY));
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
  }
  // PASSWORD_PROMPT / WIFI_SELECTION / CONNECTING render nothing of their
  // own — a child activity is on top of us in those states.

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void ReadestAuthActivity::loop() {
  if (state == NEEDS_EMAIL || state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
  }
}
