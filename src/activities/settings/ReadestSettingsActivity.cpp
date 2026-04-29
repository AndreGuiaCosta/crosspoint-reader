#include "ReadestSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <ReadestAccountStore.h>
#include <ReadestAuthClient.h>
#include <ReadestBookCatalog.h>

#include <cstdio>
#include <ctime>

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
  ITEM_LAST_SYNC,
  ITEM_LAST_ERROR,
  MENU_ITEM_COUNT,
};

bool isReadOnly(int index) { return index == ITEM_LAST_SYNC || index == ITEM_LAST_ERROR; }

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
    case ITEM_LAST_SYNC:
      return tr(STR_READEST_LAST_SYNC);
    case ITEM_LAST_ERROR:
      return tr(STR_READEST_LAST_ERROR);
    default:
      return "";
  }
}

// Render a unix-ms timestamp as a coarse relative time. Falls back to "Never"
// when the stamp is zero or in the future relative to a sane epoch (clock
// skew before NTP sync). Format mirrors typical mobile sync UIs: "Just now",
// "5m ago", "2h ago", "3d ago".
std::string formatRelativeMs(int64_t ms) {
  if (ms <= 0) return std::string(tr(STR_NEVER));
  const int64_t nowMs = static_cast<int64_t>(std::time(nullptr)) * 1000LL;
  const int64_t deltaSec = (nowMs - ms) / 1000LL;
  if (deltaSec < 0) return std::string(tr(STR_NEVER));
  char buf[32];
  if (deltaSec < 60) {
    return "Just now";
  } else if (deltaSec < 3600) {
    std::snprintf(buf, sizeof(buf), "%lldm ago", static_cast<long long>(deltaSec / 60));
  } else if (deltaSec < 86400) {
    std::snprintf(buf, sizeof(buf), "%lldh ago", static_cast<long long>(deltaSec / 3600));
  } else {
    std::snprintf(buf, sizeof(buf), "%lldd ago", static_cast<long long>(deltaSec / 86400));
  }
  return buf;
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
  if (isReadOnly(selectedIndex)) return;
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
    if (READEST_STORE.getUserEmail().empty()) return;
    startActivityForResult(std::make_unique<ReadestAuthActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) { requestUpdate(); });
  } else if (selectedIndex == ITEM_SIGN_OUT) {
    if (!READEST_STORE.hasCredentials()) return;
    ReadestAuthClient::signOut();
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
            if (READEST_STORE.getUserEmail().empty()) {
              return std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
            }
            return "";
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
          case ITEM_LAST_SYNC:
            return formatRelativeMs(READEST_STORE.getLastSyncAtMs());
          case ITEM_LAST_ERROR: {
            const auto& err = READEST_STORE.getLastSyncError();
            return err.empty() ? "" : err;
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
