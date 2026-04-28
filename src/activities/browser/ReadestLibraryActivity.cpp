#include "ReadestLibraryActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <ReadestAccountStore.h>
#include <ReadestLibraryStore.h>
#include <ReadestStorageCoordinator.h>
#include <WiFi.h>

#include <cstdio>
#include <map>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;

// Keys ending with this suffix are the cover image; everything else under
// the same book_hash group is the book file (handoff §14.4.3).
constexpr char COVER_KEY_SUFFIX[] = "/cover.png";

// Where cached cover thumbnails live. We store post-conversion BMPs (one
// per book_hash) and discard the source PNG after each conversion to keep
// SD usage low — typical Readest covers are several MB compressed but
// boil down to <1 KB at thumbnail resolution.
constexpr char COVER_CACHE_DIR[] = "/.crosspoint/readest_covers";

// Thumbnail dimensions tuned to the 30 px row height: 24×28 fits inside
// the row's selector fill and still leaves room for a 2:3 book aspect
// ratio. Text starts at ROW_TEXT_X (after the cover gutter).
constexpr int COVER_THUMB_WIDTH = 24;
constexpr int COVER_THUMB_HEIGHT = 28;
constexpr int ROW_HEIGHT = 30;
constexpr int ROW_LIST_TOP = 60;
constexpr int ROW_COVER_X = 8;
constexpr int ROW_TEXT_X = 42;
constexpr int ROW_RIGHT_PAD = 20;

bool isCoverKey(const std::string& key) {
  if (key.size() < sizeof(COVER_KEY_SUFFIX) - 1) return false;
  return key.compare(key.size() - (sizeof(COVER_KEY_SUFFIX) - 1), sizeof(COVER_KEY_SUFFIX) - 1, COVER_KEY_SUFFIX) == 0;
}

// "<userId>/Readest/Books/<hash>/cover.png" — handoff §14.5. The cover
// filename is the only piece of the file_key that's identical between S3
// and R2 deployments, so we can construct the key directly without going
// through `listFilesByBookHash` per cover.
std::string buildCoverKey(const std::string& userId, const std::string& hash) {
  return userId + "/Readest/Books/" + hash + "/cover.png";
}

std::string coverBmpPath(const std::string& hash) { return std::string(COVER_CACHE_DIR) + "/" + hash + ".bmp"; }

std::string coverImgTempPath(const std::string& hash) {
  // Single staging path regardless of format — we sniff magic bytes after
  // the GET completes and dispatch to the right decoder.
  return std::string(COVER_CACHE_DIR) + "/" + hash + ".img";
}

enum class CoverFormat { Unknown, Png, Jpeg };

// Sniff the first few bytes to figure out the encoding. Hosted Readest
// stores cover.png keys regardless of the actual upload — in practice
// most are JPEG, with the rest typically PNG. We need to detect at the
// byte level rather than trusting the key name.
CoverFormat sniffCoverFormat(FsFile& file) {
  uint8_t magic[4] = {0};
  file.seek(0);
  if (file.read(magic, sizeof(magic)) != sizeof(magic)) return CoverFormat::Unknown;
  file.seek(0);
  if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') return CoverFormat::Png;
  if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) return CoverFormat::Jpeg;
  return CoverFormat::Unknown;
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

      // Draw the cached cover thumbnail in the left gutter. A missing or
      // unreadable BMP renders as blank space — preferable to a fallback
      // icon that competes visually with the surrounding text.
      const std::string bmpPath = coverBmpPath(book.hash);
      FsFile coverFile;
      if (Storage.openFileForRead("RLIB", bmpPath.c_str(), coverFile)) {
        Bitmap bitmap(coverFile);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderer.drawBitmap(bitmap, ROW_COVER_X, rowY - 1, COVER_THUMB_WIDTH, COVER_THUMB_HEIGHT);
        }
        coverFile.close();
      }

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
  std::vector<ReadestStorageClient::BookRow> all;
  int64_t maxUpdatedAtMs = 0;
  // V1 of phase 2: full pull every time. Delta cursor (`since=last`) lands
  // alongside the already-downloaded marker in slice 5.
  const auto rc = ReadestStorageCoordinator::pullBooksSinceWithRefresh(0, &all, &maxUpdatedAtMs, &err);
  if (rc != ReadestStorageClient::OK) {
    LOG_ERR("RLIB", "books pull failed: %s (%s)", ReadestStorageClient::errorString(rc), err.c_str());
    state = State::ERROR;
    errorMessage = err.empty() ? std::string(ReadestStorageClient::errorString(rc)) : err;
    requestUpdate();
    return;
  }

  // Show only books that actually have a file in cloud storage and are not
  // soft-deleted (handoff §13.2). Local already-downloaded suppression
  // arrives in slice 5 — for now, the same book can be downloaded again.
  books.clear();
  books.reserve(all.size());
  for (auto& row : all) {
    if (row.uploadedAtMs > 0 && !row.deleted && !row.hash.empty()) {
      books.push_back(std::move(row));
    }
  }

  selectorIndex = 0;
  // Covers are downloaded synchronously here so the list renders complete
  // on first display. After first run the BMPs are cached on SD; subsequent
  // entries skip already-cached books and only fetch what's new.
  loadCovers();
  state = State::BROWSING;
  if (books.empty()) {
    // Empty library is a valid state (new account, all deleted, …) — render
    // STR_NO_ENTRIES rather than treating it as an error.
    LOG_DBG("RLIB", "books pull returned no downloadable rows");
  }
  requestUpdate();
}

void ReadestLibraryActivity::loadCovers() {
  if (books.empty()) return;
  Storage.mkdir(COVER_CACHE_DIR);

  // First pass — figure out which books still need covers. Avoids touching
  // the network on a warm cache and lets us show a useful "n/m" counter.
  std::vector<size_t> missing;
  missing.reserve(books.size());
  for (size_t i = 0; i < books.size(); i++) {
    if (!Storage.exists(coverBmpPath(books[i].hash).c_str())) {
      missing.push_back(i);
    }
  }
  if (missing.empty()) {
    LOG_DBG("RLIB", "all %u covers already cached", static_cast<unsigned>(books.size()));
    return;
  }
  LOG_DBG("RLIB", "fetching %u covers (out of %u books)", static_cast<unsigned>(missing.size()),
          static_cast<unsigned>(books.size()));

  for (size_t k = 0; k < missing.size(); k++) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Loading covers %u/%u", static_cast<unsigned>(k + 1),
                  static_cast<unsigned>(missing.size()));
    statusMessage = buf;
    requestUpdate(true);

    if (!ensureCoverCached(books[missing[k]])) {
      // Soft-fail: missing covers render as a blank gutter — no need to
      // abort the activity over a single bad cover.
      LOG_DBG("RLIB", "cover fetch failed for hash=%.8s…", books[missing[k]].hash.c_str());
    }
  }
}

bool ReadestLibraryActivity::ensureCoverCached(const ReadestStorageClient::BookRow& book) {
  const std::string bmpPath = coverBmpPath(book.hash);
  if (Storage.exists(bmpPath.c_str())) return true;

  const std::string userId = READEST_STORE.getUserId();
  if (userId.empty()) return false;
  const std::string coverKey = buildCoverKey(userId, book.hash);

  // Sign just this one key. Batching across all missing covers would be a
  // single round trip, but presigned URLs expire in 30 min — if the user
  // has hundreds of new books on a slow connection a single batch could
  // stale before we reach the last download. One sign per cover is
  // simpler and the per-call overhead is small (~100–300 ms).
  std::map<std::string, std::string> urls;
  std::string err;
  const auto rc = ReadestStorageCoordinator::getDownloadUrlsWithRefresh({coverKey}, &urls, &err);
  if (rc != ReadestStorageClient::OK) {
    LOG_DBG("RLIB", "cover sign failed for %.8s…: %s", book.hash.c_str(), err.c_str());
    return false;
  }
  const auto it = urls.find(coverKey);
  if (it == urls.end() || it->second.empty()) return false;

  // Stage to a single .img path regardless of encoding. The hosted Readest
  // stores covers at the "cover.png" key but the actual bytes can be either
  // PNG or JPEG depending on what the user uploaded — we sniff magic bytes
  // and dispatch to the right decoder. Source file is dropped after
  // conversion regardless of result; thumbnails are <1 KB so SD pressure
  // is negligible.
  const std::string imgPath = coverImgTempPath(book.hash);
  if (HttpDownloader::downloadToFile(it->second, imgPath) != HttpDownloader::OK) return false;

  bool ok = false;
  {
    FsFile imgFile;
    FsFile bmpFile;
    if (Storage.openFileForRead("RLIB", imgPath.c_str(), imgFile) &&
        Storage.openFileForWrite("RLIB", bmpPath.c_str(), bmpFile)) {
      const CoverFormat fmt = sniffCoverFormat(imgFile);
      switch (fmt) {
        case CoverFormat::Png:
          ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(imgFile, bmpFile, COVER_THUMB_WIDTH,
                                                                 COVER_THUMB_HEIGHT);
          break;
        case CoverFormat::Jpeg:
          ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(imgFile, bmpFile, COVER_THUMB_WIDTH,
                                                                   COVER_THUMB_HEIGHT);
          break;
        case CoverFormat::Unknown:
          LOG_DBG("RLIB", "cover for %.8s… is neither PNG nor JPEG — skipping", book.hash.c_str());
          break;
      }
    }
  }
  Storage.remove(imgPath.c_str());
  if (!ok) {
    Storage.remove(bmpPath.c_str());
    return false;
  }
  return true;
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

  // 2. Sign the book key. Cover signing lands in slice 4 alongside the
  //    actual cover-rendering code path; the bytes are useless to us until
  //    that side exists.
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
