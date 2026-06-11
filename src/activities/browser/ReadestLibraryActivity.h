#pragma once
#include <ReadestStorageClient.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class ReadestLibraryActivity final : public Activity {
 public:
  enum class State { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR };

  explicit ReadestLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadestLibrary", renderer, mappedInput), buttonNavigator() {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  State state = State::LOADING;
  std::vector<ReadestStorageClient::BookRow> books;
  int selectorIndex = 0;
  std::string statusMessage;
  std::string errorMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  // Set when a download is cancelled via Back: the release that cancelled
  // would otherwise immediately fire again in BROWSING and exit to home.
  bool consumeNextBackRelease = false;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  void fetchBooks();
  void downloadBook(const ReadestStorageClient::BookRow& book);
  bool preventAutoSleep() override { return true; }
};
