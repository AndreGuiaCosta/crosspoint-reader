#pragma once
#include <ReadestStorageClient.h>

#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Browse and download books from the user's Readest cloud library.
 *
 * Mirrors `OpdsBookBrowserActivity`'s state machine and download UX so the
 * two cloud-library entry points feel identical to the user. Pulls the
 * `books` table via ReadestStorageCoordinator (lazy-refresh on 401), shows
 * a flat paged list, and on Confirm performs the two-step
 * list-files-by-hash → sign-presigned-URL → GET-bytes flow before
 * dropping the EPUB into the SD root with the same `"/<title> - <author>.epub"`
 * naming convention OPDS uses.
 */
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

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchBooks();
  void downloadBook(const ReadestStorageClient::BookRow& book);
  bool preventAutoSleep() override { return true; }
};
