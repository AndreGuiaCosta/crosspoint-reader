#pragma once
#include <Epub.h>

#include <memory>
#include <optional>
#include <string>

#include "ReadestProgressMapper.h"
#include "ReadestSyncClient.h"
#include "activities/Activity.h"

// Sync reading progress with a Readest server.
// Flow: WiFi → NTP → hash → pull → user picks Apply Remote / Upload Local
// → apply or push → done. User-triggered only.
class ReadestSyncActivity final : public Activity {
 public:
  explicit ReadestSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const std::shared_ptr<Epub>& epub, const std::string& epubPath, int currentSpineIndex,
                               int currentPage, int totalPagesInSpine,
                               std::optional<uint16_t> currentParagraphIndex = std::nullopt)
      : Activity("ReadestSync", renderer, mappedInput),
        epub(epub),
        epubPath(epubPath),
        currentSpineIndex(currentSpineIndex),
        currentPage(currentPage),
        totalPagesInSpine(totalPagesInSpine),
        currentParagraphIndex(currentParagraphIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }
  bool isReaderActivity() const override { return true; }

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

  std::shared_ptr<Epub> epub;
  std::string epubPath;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string bookHash;
  std::string metaHash;

  // Remote state — populated after a successful pull.
  bool hasRemote = false;
  ReadestSyncClient::BookConfig remoteConfig;
  CrossPointPosition remotePosition;

  // Local position rendered into Readest wire format for the comparison
  // screen. Computed once at sync time so the same numbers display and push.
  ReadestPosition localReadest;

  // Selection in the comparison screen: 0 = Apply remote, 1 = Upload local.
  int selectedOption = 0;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  bool computeHashes();  // Populates bookHash + metaHash; returns false on failure with state set.
};
