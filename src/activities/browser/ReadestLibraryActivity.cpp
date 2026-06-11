#include "ReadestLibraryActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ReadestBookCatalog.h>
#include <ReadestLibraryStore.h>
#include <ReadestStorageCoordinator.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <map>

#include "MappedInputManager.h"
#include "SilentRestart.h"
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

constexpr char COVER_KEY_SUFFIX[] = "/cover.png";

bool isCoverKey(const std::string& key) {
  if (key.size() < sizeof(COVER_KEY_SUFFIX) - 1) return false;
  return key.compare(key.size() - (sizeof(COVER_KEY_SUFFIX) - 1), sizeof(COVER_KEY_SUFFIX) - 1, COVER_KEY_SUFFIX) == 0;
}

// Only EPUB opens in the reader; hide other formats rather than downloading
// files the device can't render. Rows with an empty format (older clients
// didn't always set it) stay visible.
bool isEpubFormat(std::string format) {
  if (format.empty()) return true;
  std::transform(format.begin(), format.end(), format.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return format == "epub";
}
}  // namespace

void ReadestLibraryActivity::onEnter() {
  Activity::onEnter();

  // Lazy-load (once per boot) — see main.cpp: keeps the catalog off the boot
  // path and out of resident heap for users who never open this screen.
  static bool storesLoaded = false;
  if (!storesLoaded) {
    READEST_LIB_STORE.loadFromFile();
    READEST_CATALOG.loadFromFile();
    storesLoaded = true;
  }

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
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    // WiFi+mbedTLS teardown leaves the heap fragmented; reboot to home like
    // OpdsBookBrowserActivity::onExit instead of returning on a frayed heap.
    silentRestart();
  }
}

void ReadestLibraryActivity::loop() {
  if (state == State::WIFI_SELECTION) return;

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Status-only — sim's IPAddress::operator!= always returns false.
      if (WiFi.status() == WL_CONNECTED) {
        showLoadingBeforeFetch();
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
    if (consumeNextBackRelease) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        consumeNextBackRelease = false;
        return;
      }
      // Release already consumed by the in-download input polling.
      if (!mappedInput.isPressed(MappedInputManager::Button::Back)) consumeNextBackRelease = false;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!books.empty() && !READEST_LIB_STORE.hasLocalCopy(bookAt(selectorIndex).hash)) {
        downloadBook(bookAt(selectorIndex));
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

  const bool selectedDownloaded = !books.empty() && READEST_LIB_STORE.hasLocalCopy(bookAt(selectorIndex).hash);
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
      const auto& book = bookAt(i);
      const int rowY = ROW_LIST_TOP + (i % PAGE_ITEMS) * ROW_HEIGHT;
      const bool inverted = i != static_cast<size_t>(selectorIndex);
      const bool isDownloaded = READEST_LIB_STORE.hasLocalCopy(book.hash);

      std::string displayText = book.title;
      if (!book.author.empty()) displayText += " - " + book.author;

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

void ReadestLibraryActivity::showLoadingBeforeFetch() {
  // fetchBooks() blocks on TLS for seconds; requestUpdate() alone only sets a
  // flag that is flushed after we return, so the Loading screen would never
  // paint (same fix as OpdsBookBrowserActivity::showLoadingBeforeFetch).
  state = State::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();
}

void ReadestLibraryActivity::fetchBooks() {
  std::string err;
  std::vector<ReadestStorageClient::BookRow> delta;
  int64_t maxUpdatedAtMs = 0;
  // Incremental pull: cursor=0 is a full scan, otherwise only changed rows.
  const int64_t cursor = READEST_CATALOG.getCursorMs();
  const auto rc = ReadestStorageCoordinator::pullBooksSinceWithRefresh(cursor, &delta, &maxUpdatedAtMs, &err);
  if (rc != ReadestStorageClient::OK) {
    LOG_ERR("RLIB", "books pull failed: %s (%s)", ReadestStorageClient::errorString(rc), err.c_str());
    // Offline fallback: render cached rows if we have any.
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

  books.clear();
  const auto& rows = READEST_CATALOG.getBooks();
  books.reserve(rows.size());
  for (size_t i = 0; i < rows.size() && i <= UINT16_MAX; ++i) {
    const auto& row = rows[i];
    if (row.uploadedAtMs > 0 && !row.hash.empty() && isEpubFormat(row.format)) {
      books.push_back(static_cast<uint16_t>(i));
    }
  }

  selectorIndex = 0;
  state = State::BROWSING;
  LOG_DBG("RLIB", "library ready: %u books", static_cast<unsigned>(books.size()));
  requestUpdate();
}

void ReadestLibraryActivity::downloadBook(const ReadestStorageClient::BookRow& book) {
  state = State::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

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

  // Trust the storage key for the on-disk extension — a hardcoded ".epub"
  // would record files the reader can't open under a lying name.
  std::string ext = ".epub";
  const size_t dot = bookKey.rfind('.');
  if (dot != std::string::npos && dot > bookKey.rfind('/') && bookKey.size() - dot <= 6) {
    ext = bookKey.substr(dot);
  }

  const std::string baseName =
      "/" + StringUtils::sanitizeFilename(book.title + (book.author.empty() ? "" : " - " + book.author));
  std::string filename = baseName + ext;
  if (Storage.exists(filename.c_str()) && READEST_LIB_STORE.getLocalPath(book.hash) != filename) {
    // Same sanitized title+author as a different existing file (e.g. two
    // editions) — HttpDownloader would silently replace it. Keep both.
    filename = baseName + " [" + book.hash.substr(0, 8) + "]" + ext;
  }
  LOG_DBG("RLIB", "Downloading: hash=%.8s… -> %s", book.hash.c_str(), filename.c_str());

  // This branch's HttpDownloader has no shouldCancel hook; poll input from
  // the progress callback (fires per chunk) and trip the cancelFlag it does
  // check between chunks.
  bool cancelRequested = false;
  const auto result = HttpDownloader::downloadToFile(
      it->second, filename,
      [this, &cancelRequested](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        mappedInput.update();
        if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
            mappedInput.wasPressed(MappedInputManager::Button::Back) ||
            mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          cancelRequested = true;
        }
        requestUpdate(true);
      },
      &cancelRequested);

  if (result == HttpDownloader::OK) {
    Epub(filename, "/.crosspoint").clearCache();
    READEST_LIB_STORE.recordDownload(book.hash, filename);
    state = State::BROWSING;
  } else if (result == HttpDownloader::ABORTED) {
    LOG_DBG("RLIB", "Download cancelled");
    consumeNextBackRelease = true;  // the cancelling press would otherwise fire in BROWSING
    state = State::BROWSING;
  } else {
    state = State::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void ReadestLibraryActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    showLoadingBeforeFetch();
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
    showLoadingBeforeFetch();
    fetchBooks();
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
