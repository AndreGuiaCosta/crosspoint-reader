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

#include <cassert>
#include <cstdio>
#include <ctime>

#include "Epub/Section.h"
#include "EpubReaderUtils.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
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

void ReadestSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    LOG_DBG("RSync", "Loading epub for progress mapping (heap: %u)", (unsigned)ESP.getFreeHeap());
    epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    epub->setupCacheDir();
    if (!epub->load(false, true)) {
      LOG_ERR("RSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    LOG_DBG("RSync", "Epub loaded (heap: %u)", (unsigned)ESP.getFreeHeap());
  }
}

void ReadestSyncActivity::saveProgressAndReturn(int spineIndex, int page) {
  assert(epub);
  if (!EpubReaderUtils::saveProgress(*epub, spineIndex, page, 0)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  returnToReader();
}

void ReadestSyncActivity::returnToReader() { activityManager.goToReader(epubPath); }

void ReadestSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("RSync", "WiFi connection failed, exiting");
    returnToReader();
    return;
  }

  LOG_DBG("RSync", "WiFi connected, starting sync");
  wifiActivated = true;

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // Supabase rejects skewed clocks on token refresh.
  NtpSync::syncTime();

  performSync();
}

void ReadestSyncActivity::performSync() {
  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  requestUpdateAndWait();

  // sinceMs=0 fetches any existing config, not just newer ones.
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

  // Empty bookHash means the server returned no row matching ours.
  if (pulled.bookHash.empty()) {
    RenderLock lock(*this);
    state = NO_REMOTE_PROGRESS;
    hasRemote = false;
    requestUpdate(true);
    return;
  }

  hasRemote = true;
  remoteConfig = pulled;

  // Epub was released before sync to free RAM for the TLS handshake — reload it now.
  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = "";
    }
    requestUpdate(true);
    return;
  }

  ReadestPosition rPos;
  rPos.xpointer = remoteConfig.xpointer;
  rPos.location = remoteConfig.location;
  rPos.progressCurrent = remoteConfig.progressCurrent;
  rPos.progressTotal = remoteConfig.progressTotal;
  remotePosition = ReadestProgressMapper::toCrossPoint(epub, rPos, currentSpineIndex, totalPagesInSpine);

  // Refine the page from a paragraph anchor via the section LUT.
  if (remotePosition.hasParagraphIndex) {
    Section tempSection(epub, remotePosition.spineIndex, renderer);
    const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
    if (paragraphPage.has_value()) {
      LOG_DBG("RSync", "Paragraph %u resolved to page %d (was %d)", remotePosition.paragraphIndex, *paragraphPage,
              remotePosition.pageNumber);
      remotePosition.pageNumber = *paragraphPage;
    }
  }

  const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
  remoteChapterName = (remoteTocIndex >= 0)
                          ? epub->getTocItem(remoteTocIndex).title
                          : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));

  RenderLock lock(*this);
  state = SHOWING_RESULT;
  // Default to whichever side reads further (normalized progress ratio).
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

  ReadestSyncClient::BookConfig push;
  push.bookHash = bookHash;
  push.metaHash = metaHash;
  push.xpointer = localReadest.xpointer;
  push.location = localReadest.location;  // Always empty.
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
  if (wifiActivated) {
    // WiFi+mbedTLS teardown leaves the heap fragmented; reloading the Epub
    // on it can OOM. Reboot straight back into the reader instead (same
    // recovery as KOReaderSyncActivity::onExit).
    silentRestartToReader();
  }
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

    const std::string& remoteChapter = remoteChapterName;
    const std::string& localChapter =
        localChapterName.empty() ? (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1))
                                 : localChapterName;

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
  if (state == NO_CREDENTIALS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // No epub was loaded yet — just unwind directly.
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (state == SYNC_FAILED || state == UPLOAD_COMPLETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
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
        saveProgressAndReturn(remotePosition.spineIndex, remotePosition.pageNumber);
      } else if (selectedOption == 1) {
        performUpload();
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }

  if (state == NO_REMOTE_PROGRESS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      performUpload();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToReader();
    }
    return;
  }
}
