#include "ReadestSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <ReadestAccountStore.h>
#include <ReadestAuthClient.h>
#include <ReadestBookCatalog.h>

#include "MappedInputManager.h"
#include "ReadestAuthActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
enum MenuItem {
  ITEM_EMAIL = 0,
  ITEM_SIGN_IN,
  ITEM_SIGN_OUT,
  ITEM_SYNC_API_URL,
  ITEM_SUPABASE_URL,
  MENU_ITEM_COUNT,
};

const char* itemLabel(int index) {
  switch (index) {
    case ITEM_EMAIL:
      return tr(STR_READEST_EMAIL);
    case ITEM_SIGN_IN:
      return tr(STR_READEST_SIGN_IN);
    case ITEM_SIGN_OUT:
      return tr(STR_READEST_SIGN_OUT);
    case ITEM_SYNC_API_URL:
      return tr(STR_READEST_SYNC_API_URL);
    case ITEM_SUPABASE_URL:
      return tr(STR_READEST_SUPABASE_URL);
    default:
      return "";
  }
}
}  // namespace

void ReadestSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void ReadestSettingsActivity::onExit() { Activity::onExit(); }

void ReadestSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEM_COUNT;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
    requestUpdate();
  });
}

void ReadestSettingsActivity::handleSelection() {
  if (selectedIndex == ITEM_EMAIL) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READEST_EMAIL),
                                                                   READEST_STORE.getUserEmail(), 128, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               READEST_STORE.setUserEmail(kb.text);
                             }
                           });
  } else if (selectedIndex == ITEM_SIGN_IN) {
    if (READEST_STORE.getUserEmail().empty()) return;  // No-op without email; user sees status.
    startActivityForResult(std::make_unique<ReadestAuthActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  } else if (selectedIndex == ITEM_SIGN_OUT) {
    if (!READEST_STORE.hasCredentials()) return;
    // Best-effort server-side revoke + always-clear-locally is the contract
    // documented on ReadestAuthClient::signOut.
    ReadestAuthClient::signOut();
    // Wipe the cached catalog so a different user signing in next sees their
    // own library, not the previous account's. Downloaded EPUBs and the
    // hash→path map deliberately stay on SD — the user can keep reading
    // local copies regardless of session state.
    READEST_CATALOG.clear();
    requestUpdate();
  } else if (selectedIndex == ITEM_SYNC_API_URL) {
    const std::string current = READEST_STORE.getSyncApiBaseRaw();
    const std::string prefill = current.empty() ? "https://" : current;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READEST_SYNC_API_URL),
                                                                   prefill, 192, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string toSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               READEST_STORE.setSyncApiBase(toSave);
                               READEST_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == ITEM_SUPABASE_URL) {
    const std::string current = READEST_STORE.getSupabaseUrlRaw();
    const std::string prefill = current.empty() ? "https://" : current;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READEST_SUPABASE_URL),
                                                                   prefill, 192, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string toSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               READEST_STORE.setSupabaseUrl(toSave);
                               READEST_STORE.saveToFile();
                             }
                           });
  }
}

void ReadestSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READEST_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEM_COUNT, static_cast<int>(selectedIndex),
      [](int index) { return std::string(itemLabel(index)); }, nullptr, nullptr,
      [](int index) -> std::string {
        switch (index) {
          case ITEM_EMAIL: {
            const auto email = READEST_STORE.getUserEmail();
            return email.empty() ? std::string(tr(STR_NOT_SET)) : email;
          }
          case ITEM_SIGN_IN:
            // Show readiness rather than a value.
            if (READEST_STORE.getUserEmail().empty()) {
              return std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
            }
            return READEST_STORE.hasCredentials() ? "" : "";
          case ITEM_SIGN_OUT:
            return READEST_STORE.hasCredentials() ? "" : std::string(tr(STR_NOT_SET));
          case ITEM_SYNC_API_URL: {
            const auto url = READEST_STORE.getSyncApiBaseRaw();
            return url.empty() ? std::string(tr(STR_DEFAULT_VALUE)) : url;
          }
          case ITEM_SUPABASE_URL: {
            const auto url = READEST_STORE.getSupabaseUrlRaw();
            return url.empty() ? std::string(tr(STR_DEFAULT_VALUE)) : url;
          }
          default:
            return "";
        }
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
