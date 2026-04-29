#include "ReadestLibraryActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ReadestBookCatalog.h>
#include <ReadestLibraryStore.h>
#include <ReadestStorageCoordinator.h>
#include <WiFi.h>

#include <map>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;

constexpr int ROW_HEIGHT = 30;
constexpr int ROW_LIST_TOP = 60;
constexpr int ROW_TEXT_X = 8;
constexpr int ROW_RIGHT_PAD = 20;

// Keys ending with this suffix are the cover image; everything else under
// the same book_hash group is the book file (handoff §14.4.3). We don't
// render covers in this list, but we still need to recognise them so the
// download path picks the EPUB row rather than the cover.
constexpr char COVER_KEY_SUFFIX[] = "/cover.png";

bool isCoverKey(const std::string& key) {
  if (key.size() < sizeof(COVER_KEY_SUFFIX) - 1) return false;
  return key.compare(key.size() - (sizeof(COVER_KEY_SUFFIX) - 1), sizeof(COVER_KEY_SUFFIX) - 1, COVER_KEY_SUFFIX) == 0;
}
}  // namespace

void ReadestLibraryActivity::onEnter() {
  Activity::onEnter();

  state = State::CHECK_WIFI;
  books.clear();
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  downloadProgress = downloadTotal = 0;
  requestUpdate();

  checkAndConnectWifi();
}

void ReadestLibraryActivity::onExit() {
  Activity::onExit();
  WiFi.mode(WIFI_OFF);
  books.clear();
}

void ReadestLibraryActivity::loop() {
  if (state == State::WIFI_SELECTION) return;

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Match ReadestSyncActivity: status-only check. The simulator's
      // IPAddress::operator!= always returns false so a localIP check
      // would force WIFI_SELECTION even when the libcurl-backed mock can
      // already reach the network — and on real hardware status alone is
      // sufficient to attempt a fetch.
      if (WiFi.status() == WL_CONNECTED) {
        state = State::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchBooks();
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::DOWNLOADING) return;

  if (state == State::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Suppress re-download for books already pulled via this activity.
      // The button hint disappears for these rows so the no-op matches
      // what the user sees.
      if (!books.empty() && !READEST_LIB_STORE.hasLocalCopy(books[selectorIndex].hash)) {
        downloadBook(books[selectorIndex]);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }

    if (!books.empty()) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, books.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, books.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, books.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, books.size(), PAGE_ITEMS);
        requestUpdate();
      });
    }
  }
}

void ReadestLibraryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_READEST_LIBRARY), true, EpdFontFamily::BOLD);

  if (state == State::CHECK_WIFI || state == State::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  // BROWSING — paged flat list of `books`. Confirm action is Download for
  // un-downloaded rows; for already-downloaded rows the action label is
  // hidden so users see immediately that the row is inert.
  const bool selectedDownloaded = !books.empty() && READEST_LIB_STORE.hasLocalCopy(books[selectorIndex].hash);
  const char* confirmLabel = selectedDownloaded ? "" : tr(STR_DOWNLOAD);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (books.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, ROW_LIST_TOP + (selectorIndex % PAGE_ITEMS) * ROW_HEIGHT - 2, pageWidth - 1, ROW_HEIGHT);

    const char* downloadedSuffix = tr(STR_DOWNLOADED);
    const int downloadedSuffixWidth = renderer.getTextWidth(UI_10_FONT_ID, downloadedSuffix);

    for (size_t i = pageStartIndex; i < books.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      const auto& book = books[i];
      const int rowY = ROW_LIST_TOP + (i % PAGE_ITEMS) * ROW_HEIGHT;
      const bool inverted = i != static_cast<size_t>(selectorIndex);
      const bool isDownloaded = READEST_LIB_STORE.hasLocalCopy(book.hash);

      std::string displayText = book.title;
      if (!book.author.empty()) displayText += " - " + book.author;

      // Reserve space at the right for the "Downloaded" suffix when the
      // row qualifies — otherwise the title gets the whole row.
      const int titleMaxWidth = pageWidth - ROW_TEXT_X - ROW_RIGHT_PAD - (isDownloaded ? downloadedSuffixWidth + 8 : 0);
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), titleMaxWidth);
      renderer.drawText(UI_10_FONT_ID, ROW_TEXT_X, rowY, item.c_str(), inverted);

      if (isDownloaded) {
        renderer.drawText(UI_10_FONT_ID, pageWidth - ROW_RIGHT_PAD - downloadedSuffixWidth, rowY, downloadedSuffix,
                          inverted);
      }
    }
  }
  renderer.displayBuffer();
}

void ReadestLibraryActivity::fetchBooks() {
  std::string err;
  std::vector<ReadestStorageClient::BookRow> delta;
  int64_t maxUpdatedAtMs = 0;
  // Incremental pull. `cursor=0` on first run does a full scan; subsequent
  // entries only get rows touched since the last successful merge — much
  // faster on a stable account, and the response size scales with churn
  // rather than total library size.
  const int64_t cursor = READEST_CATALOG.getCursorMs();
  const auto rc = ReadestStorageCoordinator::pullBooksSinceWithRefresh(cursor, &delta, &maxUpdatedAtMs, &err);
  if (rc != ReadestStorageClient::OK) {
    LOG_ERR("RLIB", "books pull failed: %s (%s)", ReadestStorageClient::errorString(rc), err.c_str());
    // Offline fallback: if we have cached rows, render those instead of
    // erroring out. The user can still browse and re-download books that
    // were synced before connectivity dropped.
    if (READEST_CATALOG.getBooks().empty()) {
      state = State::ERROR;
      errorMessage = err.empty() ? std::string(ReadestStorageClient::errorString(rc)) : err;
      requestUpdate();
      return;
    }
    LOG_DBG("RLIB", "pull failed, falling back to %u cached rows",
            static_cast<unsigned>(READEST_CATALOG.getBooks().size()));
  } else {
    READEST_CATALOG.mergeDelta(delta, maxUpdatedAtMs);
    LOG_DBG("RLIB", "delta pull: %u changed rows, cursor → %lld", static_cast<unsigned>(delta.size()),
            static_cast<long long>(READEST_CATALOG.getCursorMs()));
  }

  // Filter the catalog for display: only rows with a cloud file present.
  // Soft-deletes are dropped at merge time so we don't filter them again.
  books.clear();
  books.reserve(READEST_CATALOG.getBooks().size());
  for (const auto& row : READEST_CATALOG.getBooks()) {
    if (row.uploadedAtMs > 0 && !row.hash.empty()) {
      books.push_back(row);
    }
  }

  selectorIndex = 0;
  state = State::BROWSING;
  // Marker for scripted UI tests: deterministic "list is ready to render"
  // sentinel that fires once on every entry regardless of success/cache
  // path / fallback.
  LOG_DBG("RLIB", "library ready: %u books", static_cast<unsigned>(books.size()));
  requestUpdate();
}

void ReadestLibraryActivity::downloadBook(const ReadestStorageClient::BookRow& book) {
  state = State::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // 1. Discover the canonical file_keys for this book group. Always go via
  //    list rather than guessing — handoff §14.2 / §14.4.4 — because the
  //    hosted backend uses R2 and the file_keys carry a sanitized title,
  //    not the hash, so a constructed S3-style key would only work via the
  //    server's fallback path.
  std::vector<ReadestStorageClient::FileRow> files;
  std::string err;
  auto rc = ReadestStorageCoordinator::listFilesByBookHashWithRefresh(book.hash, &files, &err);
  if (rc != ReadestStorageClient::OK) {
    LOG_ERR("RLIB", "list failed: %s (%s)", ReadestStorageClient::errorString(rc), err.c_str());
    state = State::ERROR;
    errorMessage = err.empty() ? std::string(ReadestStorageClient::errorString(rc)) : err;
    requestUpdate();
    return;
  }

  std::string bookKey;
  for (const auto& f : files) {
    if (!isCoverKey(f.fileKey)) {
      bookKey = f.fileKey;
      break;
    }
  }
  if (bookKey.empty()) {
    LOG_ERR("RLIB", "no book file in list response (only cover?)");
    state = State::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  // 2. Sign the book key. Cover keys are signed separately by the cover
  //    fetch path; here we only need the EPUB.
  std::map<std::string, std::string> urls;
  err.clear();
  rc = ReadestStorageCoordinator::getDownloadUrlsWithRefresh({bookKey}, &urls, &err);
  if (rc != ReadestStorageClient::OK) {
    LOG_ERR("RLIB", "sign failed: %s (%s)", ReadestStorageClient::errorString(rc), err.c_str());
    state = State::ERROR;
    errorMessage = err.empty() ? std::string(ReadestStorageClient::errorString(rc)) : err;
    requestUpdate();
    return;
  }
  const auto it = urls.find(bookKey);
  if (it == urls.end() || it->second.empty()) {
    LOG_ERR("RLIB", "no signed URL for %s", bookKey.c_str());
    state = State::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  // 3. Stream bytes from the presigned URL to SD. Path matches OPDS
  //    convention so downloaded books appear in the file browser the same
  //    way regardless of source.
  const std::string filename =
      "/" + StringUtils::sanitizeFilename(book.title + (book.author.empty() ? "" : " - " + book.author)) + ".epub";
  LOG_DBG("RLIB", "Downloading: hash=%.8s… -> %s", book.hash.c_str(), filename.c_str());

  const auto result =
      HttpDownloader::downloadToFile(it->second, filename, [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      });

  if (result == HttpDownloader::OK) {
    Epub(filename, "/.crosspoint").clearCache();
    // Persist (hash → local path) so the marker shows on the next render
    // and re-download is suppressed across activity restarts.
    READEST_LIB_STORE.recordDownload(book.hash, filename);
    state = State::BROWSING;
  } else {
    state = State::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void ReadestLibraryActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    state = State::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchBooks();
    return;
  }
  launchWifiSelection();
}

void ReadestLibraryActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ReadestLibraryActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = State::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchBooks();
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
