#pragma once
#include <Epub.h>

#include <memory>
#include <optional>
#include <string>

#include "ReadestProgressMapper.h"
#include "ReadestSyncClient.h"
#include "activities/Activity.h"

// Sync reading progress with a Readest server.
// Flow: WiFi → NTP → pull → user picks Apply Remote / Upload Local
// → apply or push → done. User-triggered only.
//
// Hashes and the local Readest-format position are precomputed by the caller
// so this activity holds no live Epub during the TLS handshake (saves ~65KB
// of heap). The Epub is lazy-loaded after the pull to map the remote position
// onto a CrossPoint page number.
class ReadestSyncActivity final : public Activity {
 public:
  explicit ReadestSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                               int currentSpineIndex, int currentPage, int totalPagesInSpine, std::string bookHash,
                               std::string metaHash, ReadestPosition localReadest, std::string localChapterName,
                               std::optional<uint16_t> currentParagraphIndex = std::nullopt)
      : Activity("ReadestSync", renderer, mappedInput),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex),
        bookHash(std::move(bookHash)),
        metaHash(std::move(metaHash)),
        localReadest(std::move(localReadest)),
        localChapterName(std::move(localChapterName)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS,
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after the pull in performSync()
  std::string epubPath;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;

  std::string bookHash;
  std::string metaHash;
  ReadestPosition localReadest;
  std::string localChapterName;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  // WiFi+TLS fragment the heap; onExit silent-restarts back into the reader
  // when set (same recovery as KOReaderSyncActivity).
  bool wifiActivated = false;

  // Remote state — populated after a successful pull.
  bool hasRemote = false;
  ReadestSyncClient::BookConfig remoteConfig;
  CrossPointPosition remotePosition;
  std::string remoteChapterName;

  // Selection in the comparison screen: 0 = Apply remote, 1 = Upload local.
  int selectedOption = 0;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  void ensureEpubLoaded();
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();
};
