#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for Readest Sync settings. Mirrors `KOReaderSettingsActivity`.
 *
 * Items: Email, Sign In, Sign Out, Sync Server URL, Supabase URL. Email is
 * persisted to disk on save; password is not — it's collected fresh inside
 * `ReadestAuthActivity` each time the user signs in. URL fields default to
 * empty (which means "use the hosted defaults" per the account store).
 */
class ReadestSettingsActivity final : public Activity {
 public:
  explicit ReadestSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadestSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;

  void handleSelection();
};
