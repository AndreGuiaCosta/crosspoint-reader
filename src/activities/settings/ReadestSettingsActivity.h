#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Settings submenu for Readest Sync (email, sign in/out, server URLs).
// URL fields default to empty, which selects the hosted defaults.
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
