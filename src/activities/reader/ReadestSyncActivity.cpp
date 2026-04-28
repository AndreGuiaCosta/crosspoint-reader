#include "ReadestSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <NtpSync.h>
#include <ReadestAccountStore.h>
#include <ReadestHash.h>
#include <ReadestSyncCoordinator.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <cstdio>
#include <ctime>

#include "Epub/Section.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
CrossPointPosition makeLocalPositionWithParagraph(const int spineIndex, const int page, const int totalPages,
                                                  const std::optional<uint16_t>& paragraphIndex) {
  CrossPointPosition pos = {spineIndex, page, totalPages};
  if (paragraphIndex.has_value()) {
    pos.paragraphIndex = *paragraphIndex;
    pos.hasParagraphIndex = true;
  }
  return pos;
}

void wifiOff() {
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
  }
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
}  // namespace

bool ReadestSyncActivity::computeHashes() {
  bookHash = ReadestHash::partialMd5(epubPath);
  metaHash = ReadestHash::metaMd5(*epub);
  if (bookHash.empty() || metaHash.empty()) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    statusMessage = tr(STR_HASH_FAILED);
    return false;
  }
  LOG_DBG("RSync", "book_hash=%s meta_hash=%s", bookHash.c_str(), metaHash.c_str());
  return true;
}

void ReadestSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("RSync", "WiFi connection failed, exiting");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  LOG_DBG("RSync", "WiFi connected, starting sync");

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Supabase rejects skewed clocks on token refresh, so NTP must succeed
  // before any sync calls — same precondition as kosync.
  NtpSync::syncTime();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  if (!computeHashes()) {
    requestUpdate(true);
    return;
  }

  performSync();
}

void ReadestSyncActivity::performSync() {
  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // Pull with sinceMs=0 so we get any existing config, not just newer ones.
  // Coordinator handles AUTH_EXPIRED with one auto-refresh + retry.
  std::string pullErr;
  int64_t maxUpdated = 0;
  ReadestSyncClient::BookConfig pulled;
  const auto rc = ReadestSyncCoordinator::pullConfigWithRefresh(0, bookHash, metaHash, &pulled, &maxUpdated, &pullErr);

  if (rc != ReadestSyncClient::OK) {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    statusMessage = pullErr.empty() ? std::string(ReadestSyncClient::errorString(rc))
                                    : (std::string(ReadestSyncClient::errorString(rc)) + " (" + pullErr + ")");
    requestUpdate(true);
    return;
  }

  // Empty bookHash on a successful pull means: server returned no row
  // matching ours (the OR-filter union may have returned other rows that
  // matched only meta_hash; coordinator already filtered those out).
  if (pulled.bookHash.empty()) {
    RenderLock lock(*this);
    state = NO_REMOTE_PROGRESS;
    hasRemote = false;
    requestUpdate(true);
    return;
  }

  hasRemote = true;
  remoteConfig = pulled;
  ReadestPosition rPos;
  rPos.xpointer = remoteConfig.xpointer;
  rPos.location = remoteConfig.location;
  rPos.progressCurrent = remoteConfig.progressCurrent;
  rPos.progressTotal = remoteConfig.progressTotal;
  remotePosition = ReadestProgressMapper::toCrossPoint(epub, rPos, currentSpineIndex, totalPagesInSpine);

  // If the xpointer carried a paragraph index, refine the page using the
  // section cache's per-page paragraph LUT — same trick kosync uses to
  // turn a paragraph anchor into a precise page number.
  if (remotePosition.hasParagraphIndex) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
    if (paragraphPage.has_value()) {
      LOG_DBG("RSync", "Paragraph %u resolved to page %d (was %d)", remotePosition.paragraphIndex, *paragraphPage,
              remotePosition.pageNumber);
      remotePosition.pageNumber = *paragraphPage;
    }
  }

  // Cache local-side Readest representation now so the comparison screen
  // and the upload path share the same numbers.
  CrossPointPosition localPos =
      makeLocalPositionWithParagraph(currentSpineIndex, currentPage, totalPagesInSpine, currentParagraphIndex);
  localReadest = ReadestProgressMapper::toReadest(epub, localPos);

  RenderLock lock(*this);
  state = SHOWING_RESULT;
  // Default to whichever side reads further into the book — same heuristic
  // kosync uses. Both sides expose `progress=[cur,total]`, so a normalized
  // ratio is the apples-to-apples comparison.
  const float remoteRatio = remoteConfig.progressTotal > 0
                                ? static_cast<float>(remoteConfig.progressCurrent) / remoteConfig.progressTotal
                                : 0.0f;
  const float localRatio = localReadest.progressTotal > 0
                               ? static_cast<float>(localReadest.progressCurrent) / localReadest.progressTotal
                               : 0.0f;
  selectedOption = (localRatio > remoteRatio) ? 1 : 0;
  requestUpdate(true);
}

void ReadestSyncActivity::performUpload() {
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  requestUpdateAndWait();

  // Build the wire-format config from cached local Readest position. If
  // the user came in via NO_REMOTE_PROGRESS we may not have computed it
  // yet — guard against that by recomputing on the fly.
  if (localReadest.xpointer.empty() && localReadest.progressTotal == 0) {
    CrossPointPosition localPos =
        makeLocalPositionWithParagraph(currentSpineIndex, currentPage, totalPagesInSpine, currentParagraphIndex);
    localReadest = ReadestProgressMapper::toReadest(epub, localPos);
  }

  ReadestSyncClient::BookConfig push;
  push.bookHash = bookHash;
  push.metaHash = metaHash;
  push.xpointer = localReadest.xpointer;
  push.location = localReadest.location;  // Empty per V1 (handoff §7.3).
  push.progressCurrent = localReadest.progressCurrent;
  push.progressTotal = localReadest.progressTotal;
  push.updatedAtMs = static_cast<int64_t>(std::time(nullptr)) * 1000LL;

  std::string pushErr;
  ReadestSyncClient::BookConfig echo;
  const auto rc = ReadestSyncCoordinator::pushConfigWithRefresh(push, &echo, &pushErr);

  if (rc != ReadestSyncClient::OK) {
    wifiOff();
    RenderLock lock(*this);
    state = SYNC_FAILED;
    statusMessage = pushErr.empty() ? std::string(ReadestSyncClient::errorString(rc))
                                    : (std::string(ReadestSyncClient::errorString(rc)) + " (" + pushErr + ")");
    requestUpdate();
    return;
  }

  wifiOff();
  RenderLock lock(*this);
  state = UPLOAD_COMPLETE;
  requestUpdate(true);
}

void ReadestSyncActivity::onEnter() {
  Activity::onEnter();

  if (!READEST_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("RSync", "Already connected to WiFi");
    onWifiSelectionComplete(true);
    return;
  }

  LOG_DBG("RSync", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadestSyncActivity::onExit() {
  Activity::onExit();
  wifiOff();
}

void ReadestSyncActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_READEST_SYNC), true, EpdFontFamily::BOLD);

  if (state == NO_CREDENTIALS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_NO_CREDENTIALS_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, tr(STR_READEST_SETUP_HINT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (state == SHOWING_RESULT) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, tr(STR_PROGRESS_FOUND), true, EpdFontFamily::BOLD);

    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const int localTocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    const std::string localChapter =
        (localTocIndex >= 0) ? epub->getTocItem(localTocIndex).title
                             : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    // Remote — chapter, page, last-updated source/timestamp.
    renderer.drawText(UI_10_FONT_ID, 20, 160, tr(STR_REMOTE_LABEL), true);
    char remoteChapterStr[128];
    std::snprintf(remoteChapterStr, sizeof(remoteChapterStr), "  %s", remoteChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 185, remoteChapterStr);
    const float remotePct =
        remoteConfig.progressTotal > 0 ? 100.0f * remoteConfig.progressCurrent / remoteConfig.progressTotal : 0.0f;
    char remotePageStr[64];
    std::snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_PAGE_OVERALL_FORMAT), remotePosition.pageNumber + 1,
                  remotePct);
    renderer.drawText(UI_10_FONT_ID, 20, 210, remotePageStr);

    // Local — chapter, page, percentage.
    renderer.drawText(UI_10_FONT_ID, 20, 270, tr(STR_LOCAL_LABEL), true);
    char localChapterStr[128];
    std::snprintf(localChapterStr, sizeof(localChapterStr), "  %s", localChapter.c_str());
    renderer.drawText(UI_10_FONT_ID, 20, 295, localChapterStr);
    const float localPct =
        localReadest.progressTotal > 0 ? 100.0f * localReadest.progressCurrent / localReadest.progressTotal : 0.0f;
    char localPageStr[64];
    std::snprintf(localPageStr, sizeof(localPageStr), tr(STR_PAGE_TOTAL_OVERALL_FORMAT), currentPage + 1,
                  totalPagesInSpine, localPct);
    renderer.drawText(UI_10_FONT_ID, 20, 320, localPageStr);

    const int optionY = 350;
    const int optionHeight = 30;

    if (selectedOption == 0) {
      renderer.fillRect(0, optionY - 2, pageWidth - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, 20, optionY, tr(STR_APPLY_REMOTE), selectedOption != 0);

    if (selectedOption == 1) {
      renderer.fillRect(0, optionY + optionHeight - 2, pageWidth - 1, optionHeight);
    }
    renderer.drawText(UI_10_FONT_ID, 20, optionY + optionHeight, tr(STR_UPLOAD_LOCAL), selectedOption != 1);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_NO_REMOTE_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, tr(STR_UPLOAD_PROMPT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_UPLOAD), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == UPLOAD_COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, tr(STR_UPLOAD_SUCCESS), true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == SYNC_FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_SYNC_FAILED_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, statusMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ReadestSyncActivity::loop() {
  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % 2;
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedOption == 0) {
        // Apply remote — wifi torn down in onExit().
        setResult(SyncResult{remotePosition.spineIndex, remotePosition.pageNumber});
        finish();
      } else if (selectedOption == 1) {
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Hashes were computed before pull but guard anyway in case state was
      // entered through some path we haven't predicted.
      if (bookHash.empty() || metaHash.empty()) {
        if (!computeHashes()) {
          requestUpdate(true);
          return;
        }
      }
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }
}
