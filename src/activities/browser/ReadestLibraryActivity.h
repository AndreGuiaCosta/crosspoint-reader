#pragma once
#include <ReadestBookCatalog.h>
#include <ReadestStorageClient.h>

#include <cstdint>
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
  // Indices into READEST_CATALOG.getBooks() (filtered view) — a row copy per
  // book would duplicate the whole catalog on the heap while browsing.
  std::vector<uint16_t> books;
  int selectorIndex = 0;
  std::string statusMessage;
  std::string errorMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  const ReadestStorageClient::BookRow& bookAt(size_t i) const { return READEST_CATALOG.getBooks()[books[i]]; }

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  void fetchBooks();
  void downloadBook(const ReadestStorageClient::BookRow& book);
  bool preventAutoSleep() override { return true; }
};
