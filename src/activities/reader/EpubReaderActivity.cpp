#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "../../util/BookmarkFile.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "PageFlipRadio.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderFontSizes.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

namespace {
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
bool xteinkClassPanel() { return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || BoardConfig::isX4Classic(); }

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

EpubReaderActivity::~EpubReaderActivity() {
#ifdef FREEINK_CAP_PAGEFLIP
  pageflipEnd();
#endif
  ImageBlock::setExtractor(nullptr, nullptr);
  discardOverlayPage();  // free the overlay's page snapshot if one is held

  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

bool EpubReaderActivity::loadBook() {
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (!loaded) {
    LOG_ERR("ERS", "Failed to load EPUB");
    return false;
  }
  epub = std::move(loadedEpub);

#ifdef FREEINK_CAP_PAGEFLIP
  pageflipBegin();
#endif

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  loadCachedBookmarks();
  return true;
}

void EpubReaderActivity::openReaderMenu() {
  pendingManualTurn = 0;
  if (usesToolbarMenu()) {
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                             SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
                         [this](const ActivityResult& result) {
                           const auto& menu = std::get<MenuResult>(result.data);
                           if (SETTINGS.orientation != menu.orientation) {
                             applyOrientation(menu.orientation);
                           }
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                        orientedMarginLeft, orientedMarginTop),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void EpubReaderActivity::loop() {
  if (!epub) {
    finish();
    return;
  }

#ifdef FREEINK_CAP_PAGEFLIP
  // Pumped before anything else this frame so a peer's turn lands in the same pass a local press
  // would have, and so steps owed from a boundary crossing settle before the page is drawn.
  pageflipPump();
#endif

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (appliedOrientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  if (section && !section->isBuilding() && !RenderLock::peek() && renderer.hasFrameBuffer() &&
      lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
      ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock;
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);
            scope.endScanAndPrewarm();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          }
        }
      }
    }
  }

  if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
      !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock;
    const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
    if (!section->startBuild(buildSpec)) {
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  // A paired reader that has put its radio down for this build must finish it, not stop at the
  // window: the link cannot come back until the build is over (a live BuildContext holds the heap
  // under the tick floor, and the pair deadlocks -- see pageflipColdBuildOver), so stopping early
  // would strand it. The ~60 KB the radio gave back is what makes finishing affordable.
  if (section && section->isBuilding() && !RenderLock::peek() &&
      (section->isPartial() || pageflipBuildingForSuspendedLink() ||
       static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
      buildTickHeapGate()) {
    RenderLock lock;
    if (section->isBuilding() && buildTickHeapGate()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        requestUpdate();
      }
    }
  }

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

#ifdef FREEINK_CAP_PAGEFLIP
  // The force-sync prompt owns input while it is up (section 5.1). Ahead of every other handler
  // because Confirm normally opens the reader menu, and here it answers a question this device
  // asked -- and because a page turn applied mid-exchange would land on a layout about to be
  // rebuilt. Placed after the build and prewarm work above so a rebuild in flight keeps running.
  if (pageflipSyncState != PageFlipSyncState::None && pageflipHandleSyncInput()) return;
#endif

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      // Hold the interval at zero elapsed so closing the panel starts a fresh one.
      lastPageTurnTime = millis();
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      requestUpdate();
      return;
    }
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }
  const bool endOfBookMenuOpen = endOfBookMenuActive();

  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmLongPressed) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (launchKOReaderSync()) {
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        openDictionaryWordSelect();
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Home-key boards have no front Confirm button, so a Home-key hold runs the
  // same user-selected long-press action. The SDK emits this event once per
  // hold and suppresses the short Home tap for the same contact.
  if (mappedInput.wasHomeKeyHold() && !endOfBookMenuOpen) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case CrossPointSettings::LP_MENU_KOSYNC:
        launchKOReaderSync();
        return;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (!showDictionaryMessage) {
          openDictionaryWordSelect();
        }
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
        if (usesToolbarMenu() && section) {
          openOverlay(Overlay::Toolbar);
        } else {
          openReaderMenu();
        }
        return;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Link taps take priority over the reader-menu and page-turn zones.
  if (!atEndOfBook && !currentPageLinks.empty() && SETTINGS.touchReaderControls && mappedInput.hasTouch()) {
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
      const auto* link = EpubReaderUtils::linkAtPoint(currentPageLinks, touchX, touchY, currentPageLinkMarginLeft,
                                                      currentPageLinkMarginTop);
      if (link) {
        navigateToHref(link->href, true);
        return;
      }
    }
  }

  if (confirmReleased || ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && section) {
      pendingManualTurn = 0;
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
  }

  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section) {
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    pageTurn(forward);
    requestUpdate();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs >= ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    skipPages(nextTriggered ? 1 : -1);
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (!section) {
    requestUpdate();
    return;
  }

  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
  requestUpdate();
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) return;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
        RenderLock lock;
        clearDeferredReposition();
        if (section && currentSpineIndex == sync.spineIndex) {
          const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
          section->currentPage = page.value_or(std::max(0, sync.page));
        } else {
          currentSpineIndex = sync.spineIndex;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);
          section.reset();
        }
        requestUpdate();
        return;
      }

      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      RenderLock lock;
      clearDeferredReposition();

      if (currentSpineIndex != targetSpineIndex) {
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
      requestUpdate();
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      // Release the section while the chapter list is up (mirrors the
      // TEXT_SETTINGS path): picking a chapter resets it anyway, and its
      // tens-of-KB footprint is the difference between the chapter list
      // holding its CJK glyph arena (RAM-only repaints) and re-reading
      // glyphs from SD on every row step. Cancel restores via the same
      // cached-position rebuild TEXT_SETTINGS uses.
      {
        RenderLock lock;
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
      }
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, spineIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock;
            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;
            pendingAnchor = chapterResult.anchor;
            nextPageNumber = 0;
            section.reset();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 openReaderMenu();
                                 return;
                               }
                               const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                               navigateToHref(footnoteResult.href, true);
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               {
                                 RenderLock lock;
                                 if (section) {
                                   rememberCurrentContentOffset();
                                   cachedSpineIndex = currentSpineIndex;
                                   cachedChapterTotalPageCount = section->pageCount;
                                   nextPageNumber = section->currentPage;
                                 }
                                 section.reset();
                               }
                               openReaderMenu();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult&) { openReaderMenu(); });
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  switch (SETTINGS.longPressMenuFunction) {
    case CrossPointSettings::LP_MENU_BOOKMARK:
    case CrossPointSettings::LP_MENU_DICTIONARY:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case CrossPointSettings::LP_MENU_KOSYNC:
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case CrossPointSettings::LP_MENU_READER_MENU:
    case CrossPointSettings::LP_MENU_DISABLED:
    default:
      return 0;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock;
    if (section) {
      nextPageNumber = section->currentPage;
    }
    ImageBlock::setExtractor(nullptr, nullptr);
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;
}

void EpubReaderActivity::applyInitialOrientation() {
  ReaderActivity::applyInitialOrientation();
  appliedOrientation = SETTINGS.orientation;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

// Moves the position one page, without touching the page-turn timer or requesting a render, so
// callers that need several steps (or none of the turn bookkeeping) can reuse the boundary logic.
// `moved` reports whether the position actually changed, which is what pageTurn owes its caller.
// The return value answers a different question: whether stepping may continue immediately. A step
// that crosses a section boundary unloads the section, and the landing page is not known until
// render() has loaded the neighbouring one, so a caller owed further steps must defer them.
bool EpubReaderActivity::advanceOnePage(bool isForwardTurn, bool& moved) {
  moved = false;
  if (isForwardTurn) {
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
      moved = true;
      return true;
    }
    if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock;
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
      moved = true;
#ifdef FREEINK_CAP_PAGEFLIP
      pageflipSuspendForColdBuild();
#endif
      return false;
    }
    // Past the last spine is the end of the book, which ReaderActivity draws instead of a page.
    // There is nothing further to step onto, so an owed remainder is dropped here.
    currentSpineIndex = epub->getSpineItemsCount();
    moved = true;
    return false;
  }

  if (section->currentPage > 0) {
    section->currentPage--;
    moved = true;
    return true;
  }
  if (currentSpineIndex > 0) {
    {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
    }
    moved = true;
#ifdef FREEINK_CAP_PAGEFLIP
    pageflipSuspendForColdBuild();
#endif
    return false;
  }
  return false;
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (!section) return false;
  {
    RenderLock lock;
    clearDeferredReposition();
  }
#ifdef FREEINK_CAP_PAGEFLIP
  if (pageflip && pageflipPeerPresent) {
    // Paired: this device owns its own position and advances it by two, so the peer's page never
    // has to be computed from ours -- and a press on either device turns the spread the same way.
    announceWhenSettled = true;
    const bool moved = applyAdvance(isForwardTurn, 2);
    if (moved) lastPageTurnTime = millis();
    return moved;
  }
#endif
  bool moved = false;
  advanceOnePage(isForwardTurn, moved);
  if (moved) lastPageTurnTime = millis();
  return moved;
}

#ifdef FREEINK_CAP_PAGEFLIP
// Returns whether the position actually moved, which is what pageTurn() owes ReaderActivity. A
// press at the very start of the book moves nothing, and reporting a turn there would tell the base
// a page had been left when the reader is still on it.
bool EpubReaderActivity::applyAdvance(bool forward, uint8_t steps) {
  // The divergent prompt describes the page this device was on when the join classified it. Once
  // the pair has moved -- pressed here, or pressed on the peer and arriving as a turn -- that
  // sentence is about a page nobody is looking at, and confirming it would propose a position the
  // user never saw. Reading on is an answer.
  if (pageflipSyncState == PageFlipSyncState::Resuming) pageflipSetSyncState(PageFlipSyncState::None);

  pendingAdvanceForward = forward;
  pendingAdvanceSteps = steps;
  return consumePendingAdvance();
}

// Returns whether any step moved. Steps still owed when a boundary is crossed do not count against
// it: they are deferred, not refused, and the ones already applied did move the reader.
bool EpubReaderActivity::consumePendingAdvance() {
  bool movedAny = false;
  while (pendingAdvanceSteps > 0 && section) {
    --pendingAdvanceSteps;
    // A boundary crossing unloads the section: the rest is owed until render() has loaded the
    // neighbour, because the landing page does not exist yet to step from.
    bool moved = false;
    const bool mayContinue = advanceOnePage(pendingAdvanceForward, moved);
    movedAny = movedAny || moved;
    if (!mayContinue) return movedAny;
  }
  return movedAny;
}

void EpubReaderActivity::pageflipHealTo(const PageFlipDecision& decision) {
  {
    // Same care as a chapter jump: the section must not be dropped mid-render.
    RenderLock lock(*this);
    currentSpineIndex = decision.spineIndex;
    nextPageNumber = decision.pageNumber;
    pendingPageJump.reset();
    section.reset();
  }
  pageflipSuspendForColdBuild();
  // The role offset is a step, not arithmetic: a section boundary may sit between the peer's page
  // and ours. It is applied by the pump once the section is back.
  pendingAdvanceForward = decision.forward;
  pendingAdvanceSteps = decision.applyRoleOffset ? 1 : 0;
  requestUpdate();
}

PageFlipRole EpubReaderActivity::pageflipConfiguredRole() {
  return SETTINGS.pageflipRole == CrossPointSettings::PAGEFLIP_ROLE_RIGHT ? PageFlipRole::Right : PageFlipRole::Left;
}

bool EpubReaderActivity::pageflipApplyPeerMac() {
  if (!pageflip) return false;
  uint8_t mac[PageFlipTransport::MAC_BYTES] = {};
  // An unset or malformed setting is "no paired device", never a MAC of zeroes: a device that
  // believed it was paired with 00:00:00:00:00:00 would listen to nobody at all, which looks
  // exactly like a peer that is switched off.
  if (!PageFlipMac::parse(SETTINGS.pageflipPeerMac, mac)) {
    pageflip->setPeerMac(nullptr);
    return false;
  }
  pageflip->setPeerMac(mac);
  return true;
}

bool EpubReaderActivity::pageflipBadgeDue() const {
  if (!SETTINGS.pageflipEnabled || pageflipPeerPresent) return false;
  // A pair configured with no link at all -- suspended for WiFi (section 6), or a radio that would
  // not start -- is not waiting on anybody's answer, so it says so at once. The grace period below
  // exists for a greeting still in flight, and here there is no greeting in flight.
  if (!pageflip) return true;
  // Not while a peer might still be answering. A greeting takes a moment to come back, so the first
  // render of every paired book open would otherwise carry the badge and immediately have to lose
  // it -- a second full e-ink refresh, on every open, to un-say something that was never true.
  return millis() - pageflipLinkUpMs >= PEER_PRESENCE_TIMEOUT_MS;
}

void EpubReaderActivity::pageflipBegin() {
  if (!epub) return;
  // Off by default, and the gate is the point: bringing the link up costs a radio that transmits on
  // a timer, and almost nobody has a second X4 to pair it with.
  if (!SETTINGS.pageflipEnabled) return;

  // Section 6. One radio, one channel: ESP-NOW peers have to sit on the same channel, and
  // associating with an AP lets the AP choose it. Coming up here would do worse than fail, because
  // the SDK transport's begin() runs its own end() first -- which disconnects WiFi and puts the
  // mode back to off. A book opened during a download would silently kill the download.
  //
  // Re-asked every pump rather than once, and that is the whole of the resume path: the link comes
  // back when WiFi goes away, re-pinning the channel by construction, because begin() sets it.
  // And not before the first page is on screen. Opening a book on a cold cache is the other way
  // into the build above, and the 60 KB the link costs is worth more to that build than to a pair
  // that has nothing to say yet: the first greeting waits for a render anyway, because the layout
  // fingerprint is a render() output. buildViewportWidth is that output, so zero means no render
  // has landed. Retried every pump by pageflipReconcileSettings, which is the whole resume path.
  if (buildViewportWidth == 0) return;

  if (pageflipWifiHoldsRadio()) {
    if (!pageflipWifiSuspended) {
      pageflipWifiSuspended = true;
      LOG_INF("ERS", "WiFi has the radio; paired reading is suspended until it is done");
      // Said, not merely shown. The badge alone would report a missing peer, which is a different
      // problem with a different fix -- the user would go looking for the other device.
      snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_WIFI_PAUSED));
      pageflipSetSyncState(PageFlipSyncState::Reporting);
    }
    return;
  }
  if (pageflipWifiSuspended) {
    pageflipWifiSuspended = false;
    LOG_INF("ERS", "WiFi released the radio; bringing the paired link back");
  }

  pageflipTransport = makePageFlipTransport();
  if (!pageflipTransport) {
    LOG_ERR("ERS", "OOM: PageFlip transport");
    return;
  }
  auto session = makeUniqueNoThrow<PageFlipSession>(*pageflipTransport, pageflipConfiguredRole());
  if (!session) {
    LOG_ERR("ERS", "OOM: PageFlip session");
    pageflipTransport.reset();
    return;
  }
  if (!session->begin()) {
    // No link is the ordinary solo case, not a failure: reading must never wait on the pair.
    LOG_INF("ERS", "PageFlip link unavailable, reading solo");
    pageflipTransport.reset();
    return;
  }

  // The cache path already embeds the path hash the rest of the reader identifies a book by.
  pageflipBookId = static_cast<uint32_t>(std::hash<std::string>{}(epub->getCachePath()));
  // The layout fingerprint stays at the not-computed sentinel until the first render fixes the
  // viewport, and no greeting goes out before then: a hello carrying zero would claim a layout this
  // device has not decided on, and would read as a mismatch to any peer that had already rendered.
  // pageflipRefreshCompat() sends the first one.
  session->setBook(pageflipBookId, 0);
  pageflipLinkUpMs = millis();
  pageflip = std::move(session);
  // Before the first packet can arrive, so a stranger's greeting is never even the first thing this
  // session hears.
  const bool paired = pageflipApplyPeerMac();
  LOG_INF("ERS", "PageFlip link up as %s, %s", pageflipConfiguredRole() == PageFlipRole::Left ? "left" : "right",
          paired ? "listening for the paired device" : "waiting for a peer");
}

void EpubReaderActivity::pageflipEnd() {
  pageflip.reset();
  pageflipTransport.reset();  // releases the radio state: a solo reader pays nothing for the link
  pendingAdvanceSteps = 0;
  announceWhenSettled = false;
  pageflipPeerPresent = false;
  pageflipCompatHash = 0;
  pageflipCompatMismatch = false;
  pageflipOweJoinDecline = false;
  pageflipJoinProbePending = false;
  pageflipMismatchSinceMs = 0;
  pageflipSyncState = PageFlipSyncState::None;
  pendingPageflipSyncNotice = false;
  pageflipSyncStateSinceMs = 0;
  pageflipSyncMessage[0] = '\0';
  pageflipResumeOwnOffset = 0;
  pageflipPeerWasLost = false;
  pageflipLastHeartbeatMs = 0;
  pageflipLastDiscoveryMs = 0;
  pageflipLinkUpMs = 0;
  pageflipBadgeVisible = false;
  pageflipCachedOffsetSpine = -1;
  pageflipCachedOffsetPage = -1;
  pageflipHaveLastAnchor = false;
  pageflipHeapSuspended = false;
}

bool EpubReaderActivity::pageflipSettledPage(int32_t& spineIndex, int& page) {
  // peek() first so the main task never blocks behind a page render; the lock is then taken for the
  // read itself, because peek() alone leaves the window open for render() to start and reset the
  // section under us. Same shape as the deferred partial-extension start in loop().
  if (RenderLock::peek()) return false;
  RenderLock lock(*this);
  // The spine index comes out under the same lock as the page. render() writes it -- it clamps the
  // value on the way in, and the end-of-book panel is chosen from it -- so a caller reading it
  // afterwards would be reading a variable the render task owns, and would pair a page from one
  // moment with a chapter from another.
  spineIndex = currentSpineIndex;
  page = section ? section->currentPage : nextPageNumber;
  return true;
}

bool EpubReaderActivity::pageflipSettledPosition(int32_t& spineIndex, int& page, uint32_t& visibleTextOffset) {
  if (RenderLock::peek()) return false;
  RenderLock lock(*this);
  if (!section) return false;
  // Under the lock with the page, for the reason given in pageflipSettledPage.
  spineIndex = currentSpineIndex;
  page = section->currentPage;
  if (page < 0 || page >= section->pageCount) return false;
  // Cached per position: the heartbeat asks this every couple of seconds, and the lookup below is a
  // file read once the chapter has finalized. The cache is dropped whenever the pagination moves
  // (pageflipRefreshCompat), which is the only way an answer for the same page could change.
  if (spineIndex == pageflipCachedOffsetSpine && page == pageflipCachedOffsetPage) {
    visibleTextOffset = pageflipCachedOffset;
    return true;
  }
  // Read the same way saveProgress and rememberCurrentContentOffset read it: an in-memory lookup
  // while the section is building, one small file read otherwise.
  const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page));
  if (!offset.has_value()) return false;
  pageflipCachedOffsetSpine = spineIndex;
  pageflipCachedOffsetPage = page;
  pageflipCachedOffset = *offset;
  visibleTextOffset = *offset;
  return true;
}

bool EpubReaderActivity::pageflipPresenceAnchor(int32_t& spineIndex, int& page, uint32_t& visibleTextOffset) {
  if (pageflipSettledPosition(spineIndex, page, visibleTextOffset)) {
    pageflipRecordAnchor(spineIndex, page, visibleTextOffset);
    return true;
  }

  // No section to ask. That is not a transient state at end of book: render() takes the end-panel
  // early return before anything is loaded, and the forward crossing that got here already reset
  // the section, so it stays null for as long as the reader sits there. Requiring a fresh anchor
  // would silence this device permanently and the peer would call it offline four heartbeats later
  // -- taking down the one arrangement section 3 deliberately designs for, where the right half
  // shows the end panel and interaction stays on the left.
  //
  // The anchor only ever feeds a join probe, and a device with no section has nothing to join on,
  // so the last one read is good enough to keep saying "still here".
  if (pageflipSettledPage(spineIndex, page)) {
    pageflipRecordAnchor(spineIndex, page, pageflipCachedOffset);
    visibleTextOffset = pageflipLastAnchorOffset;
    return true;
  }

  // Nothing could be read at all, which here means one thing: the render task holds the lock. That
  // is not a reason to fall silent. A render is the one activity that proves this device is alive,
  // and on real e-ink it lasts longer than the peer's patience -- so answer from the last anchor
  // rather than skipping the heartbeat and letting a busy device be declared gone.
  //
  // The position is a few seconds old, which is why this is allowed only once the join has
  // resolved. A resolved join ignores the positions in later greetings; an unresolved one
  // classifies the pair from them -- and section 4.2's Identical case is a direct equality test on
  // exactly this pair of values, so a stale anchor there could make the spread from a page this
  // device has already left. While the join is still running both devices are greeting each other
  // anyway, so falling silent for the length of one render costs nothing the round does not retry.
  if (!pageflip->isJoinResolved()) return false;
  if (!pageflipHaveLastAnchor) return false;
  spineIndex = pageflipLastAnchorSpine;
  page = pageflipLastAnchorPage;
  visibleTextOffset = pageflipLastAnchorOffset;
  return true;
}

bool EpubReaderActivity::pageflipSectionCacheMissing(const int spineIndex) const {
  if (!epub || spineIndex < 0) return false;
  // Same path Section builds for itself. Asked here rather than inferred, because "will this
  // chapter have to be laid out" is the only thing that separates a crossing worth paying for from
  // an ordinary one: with the link up a device is always under the floor below, so a heap test on
  // its own would put the pair down at every chapter of every book.
  char path[160];
  snprintf(path, sizeof(path), "%s/sections/%d.bin", epub->getCachePath().c_str(), spineIndex);
  return !Storage.exists(path);
}

void EpubReaderActivity::pageflipSuspendForStalledBuild() {
  if (!pageflip || pageflipHeapSuspended || !pageflip->isStarted()) return;
  // The backstop for every build this reader starts anywhere other than a chapter crossing: loop()
  // resuming a partial's extension near its watermark, and render()'s blocking extension when the
  // reader crosses that watermark. Both call startBuild() and leave the BuildContext alive, so with
  // the radio resident they reach the same deadlock the crossing path does -- ~45 KB of context on
  // top of ~60 KB of radio puts free heap under BACKGROUND_BUILD_MIN_FREE_HEAP, buildTickHeapGate()
  // then refuses every tick, and the only thing that would free the heap is the build it is
  // refusing. Guarding the two call sites instead would need the same test in both and would still
  // miss the next one; this asks the question the deadlock actually turns on.
  // Heap first: it is a plain read, and it rejects almost every pump without touching the section.
  if (ESP.getFreeHeap() >= BACKGROUND_BUILD_MIN_FREE_HEAP) return;
  // `section` belongs to the render task, so it is never read from the pump unguarded -- doing that
  // segfaulted the left sim half on nearly every run, and did not reproduce under gdb.
  if (RenderLock::peek()) return;
  {
    RenderLock lock(*this);
    if (!section || !section->isBuilding()) return;
  }

  LOG_INF("ERS", "Build stalled for want of heap; releasing the paired link to finish it");
  pageflipHeapSuspended = true;
  pageflipColdBuildSpine = currentSpineIndex;
  pageflipColdBuildStartMs = millis();
  pageflip->end();
}

void EpubReaderActivity::pageflipSuspendForColdBuild() {
  if (!pageflip || pageflipHeapSuspended) return;
  if (!pageflipSectionCacheMissing(currentSpineIndex)) return;
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap >= PAGEFLIP_COLD_BUILD_MIN_FREE_HEAP) return;

  // The session survives; only the transport goes. That distinction is the design: tearing the
  // session down would reset turnSeq and the join, so every cold chapter would re-negotiate the
  // spread -- and two halves that finish building at different moments classify as Divergent, which
  // is a resume prompt in front of the user at every chapter boundary.
  //
  // The cost, stated plainly: a radio that is off cannot say "still here", so a build longer than
  // PEER_PRESENCE_TIMEOUT_MS drops presence on the peer, and the first press after that advances
  // one page instead of two. That is the flap this reader works hard to avoid everywhere else, and
  // it is still the better trade -- the alternative on this path is abort() and a reboot.
  LOG_INF("ERS", "Low heap (%u bytes); releasing the paired link to build the chapter",
          static_cast<unsigned>(freeHeap));
  pageflipHeapSuspended = true;
  // Latched here, where the answer still exists. The resume cannot work this out for itself: the
  // section was dropped a few lines ago and the reader looks the same whether the build is finished,
  // failed, or has not started.
  pageflipColdBuildSpine = currentSpineIndex;
  pageflipColdBuildStartMs = millis();
  pageflip->end();
}

bool EpubReaderActivity::pageflipColdBuildOver() {
  // A held lock IS the answer: render() keeps it for the whole pass, and loop() drives the rest of a
  // windowed build in chunks between passes. So "someone is holding it" means the work this link
  // stood down for is still running, and nothing else needs to be asked.
  if (RenderLock::peek()) return false;
  RenderLock lock(*this);
  // The reader moved on -- a jump, a heal, the book closing. Whatever was being waited for is over.
  if (currentSpineIndex != pageflipColdBuildSpine) return true;
  if (!section) {
    // Two very different states share this one appearance: render() has not built the new section
    // yet (milliseconds), or the build failed and reset it (which reports nothing else at all, and
    // is permanent). Only elapsed time separates them.
    return millis() - pageflipColdBuildStartMs >= PAGEFLIP_COLD_BUILD_START_GRACE_MS;
  }
  // The build being over is the whole answer, and it has to be this literal one. An earlier version
  // asked the weaker question "has the build done enough for now" (the window test loop() uses to
  // decide whether to tick, negated) and resumed on that. It deadlocked the device: the radio came
  // back on top of a live BuildContext, and that context holds ~45 KB, which is enough to put free
  // heap under BACKGROUND_BUILD_MIN_FREE_HEAP -- the floor buildTickHeapGate() needs to advance the
  // very build that would free it. Measured on two X4s 2026-09-05: 4,896 B free, not one further
  // "Page N processed" in 55 s, and a chapter that could never finish. buildTickHeapGate()'s own
  // comment names this state ("indefinitely, if the build context itself keeps the heap low").
  //
  // While the link is down there is ~60 KB more to work with, so loop() ticks this build past its
  // window (see the pageflipHeapSuspended term there) and it finishes in seconds rather than idling.
  return !section->isBuilding();
}

bool EpubReaderActivity::pageflipColdBuildOverdue() const {
  return pageflipHeapSuspended && millis() - pageflipColdBuildStartMs >= PAGEFLIP_COLD_BUILD_DEADLINE_MS;
}

void EpubReaderActivity::pageflipReleaseOverdueBuild() {
  // BUILD_WINDOW_AHEAD's comment states the unbounded case as a design fact: "a giant single-spine
  // book therefore never finalizes its .bin in one sitting". Waiting for isBuilding() to clear would
  // hold the pair down for the rest of that book, so past the deadline the build is put down instead
  // of waited on. suspendBuild() persists the pages already laid out as a partial and frees the
  // BuildContext, which is the point: the radio must never come back on top of a live build.
  if (RenderLock::peek()) return;
  RenderLock lock(*this);
  if (!section || !section->isBuilding()) return;
  LOG_INF("ERS", "Chapter build too long for a paired reader; suspending it to bring the link back");
  section->suspendBuild();
}

void EpubReaderActivity::pageflipResumeAfterColdBuild() {
  if (!pageflip || !pageflipHeapSuspended) return;
  // Not while the build is still running: the whole point was to keep the heap out of its way, and
  // a background build carries on for pages after the one being read is on screen.
  //
  // Asking "is the section building" directly does not work, and on hardware it defeated the whole
  // guard: advanceOnePage() resets the section and suspends in the same breath, so the very next
  // pump sees a NULL section -- which is not "building" -- and a free heap that looks healthy
  // precisely because the radio has just left. Measured 2026-09-05: the link came back 43 ms after
  // it went down, before the build had begun, and the chapter was laid out with the radio resident
  // anyway (bottomed at 5,084 B free, and the build failed).
  if (pageflipColdBuildOverdue()) pageflipReleaseOverdueBuild();
  const uint32_t freeHeap = ESP.getFreeHeap();
  const bool built = pageflipColdBuildOver();
  if (!built || freeHeap < PAGEFLIP_COLD_BUILD_RESUME_MIN_FREE_HEAP) {
    // Which gate is holding, at most once every few seconds. Without this the two waits are
    // indistinguishable in a log -- a link that stays down says nothing at all -- and working out
    // which one it was cost a whole bench run and a reflash.
    if (millis() - pageflipColdBuildWaitLogMs >= PAGEFLIP_COLD_BUILD_WAIT_LOG_MS) {
      pageflipColdBuildWaitLogMs = millis();
      LOG_DBG("ERS", "Paired link still down: build %s, free %u bytes", built ? "done" : "running",
              static_cast<unsigned>(freeHeap));
    }
    return;
  }
  // Left latched when begin() fails, so this is re-asked next pump rather than the pair being lost
  // for the session.
  if (!pageflip->begin()) return;
  pageflipHeapSuspended = false;
  pageflipColdBuildSpine = -1;
  LOG_INF("ERS", "Chapter built; bringing the paired link back");
}

void EpubReaderActivity::pageflipRecordAnchor(const int32_t spineIndex, const int page,
                                             const uint32_t visibleTextOffset) {
  pageflipLastAnchorSpine = spineIndex;
  pageflipLastAnchorPage = page;
  pageflipLastAnchorOffset = visibleTextOffset;
  pageflipHaveLastAnchor = true;
}

bool EpubReaderActivity::pageflipJoinAnswerInputs(const int32_t peerSpineIndex, const uint32_t peerVisibleTextOffset,
                                                 int& page, uint32_t& visibleTextOffset,
                                                 PageFlipJoinVerdict& verdict) {
  if (RenderLock::peek()) return false;
  RenderLock lock(*this);
  if (!section) return false;
  page = section->currentPage;
  if (page < 0 || page >= section->pageCount) return false;
  const auto ownOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page));
  if (!ownOffset.has_value()) return false;
  visibleTextOffset = *ownOffset;

  // The one question this device can answer without loading anything: does my next page start where
  // the peer says it is? Testing the other direction -- whether the PEER's next page is mine --
  // would mean laying out a section this device does not have, possibly a whole chapter, which is
  // why section 4.2 has each device test only forward from itself and combines the two answers.
  const int nextPage = page + 1;
  if (nextPage < section->pageCount) {
    if (peerSpineIndex != currentSpineIndex) {
      verdict = PageFlipJoinVerdict::NotAdjacent;
      return true;
    }
    const auto nextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(nextPage));
    verdict = !nextOffset.has_value()              ? PageFlipJoinVerdict::Unknown
              : *nextOffset == peerVisibleTextOffset ? PageFlipJoinVerdict::Adjacent
                                                     : PageFlipJoinVerdict::NotAdjacent;
    return true;
  }

  // Past the last page laid out. Whether that is the last page of the CHAPTER is a different
  // question, and one a section still building cannot answer: pageCount is a watermark until it
  // finalizes (section 3), so "there is no next page" would be a guess. Unknown means ask again,
  // and the build settling is what makes the retry produce an answer.
  if (section->isBuilding() || section->isPartial()) {
    verdict = PageFlipJoinVerdict::Unknown;
    return true;
  }

  // This really is the chapter's last page, so this device's next page is page 0 of the next spine
  // -- which it must not load to check. It does not have to: page 0 of any section starts at offset
  // 0, so the peer's own report answers it. Without this branch an ordinary aligned pair straddling
  // a chapter boundary could never answer Adjacent, and would be classified divergent forever.
  verdict = (peerSpineIndex == currentSpineIndex + 1 && peerVisibleTextOffset == 0)
                ? PageFlipJoinVerdict::Adjacent
                : PageFlipJoinVerdict::NotAdjacent;
  return true;
}

void EpubReaderActivity::pageflipAnswerJoinProbe() {
  int page = 0;
  uint32_t offset = 0;
  PageFlipJoinVerdict verdict = PageFlipJoinVerdict::Unknown;
  if (!pageflipJoinAnswerInputs(pageflipJoinPeerSpineIndex, pageflipJoinPeerOffset, page, offset, verdict)) return;

  pageflipJoinProbePending = false;
  PageFlipJoinResolution resolution;
  if (!pageflip->answerJoin(pageflipJoinRound, verdict, currentSpineIndex, page, offset, resolution)) return;
  if (!resolution.resolved) return;

  LOG_INF("ERS", "PageFlip join: %s (peer at spine %d offset %u)",
          resolution.joinCase == PageFlipJoinCase::Identical  ? "same page, making the spread"
          : resolution.joinCase == PageFlipJoinCase::Aligned  ? "already a spread"
          : resolution.joinCase == PageFlipJoinCase::Swapped  ? "devices swapped"
                                                             : "positions unrelated",
          resolution.peerSpineIndex, static_cast<unsigned>(resolution.peerVisibleTextOffset));
  pageflipApplyJoin(resolution, page, offset);
}

void EpubReaderActivity::pageflipSeekToOffset(const int32_t spineIndex, const uint32_t visibleTextOffset) {
  {
    // Same care as a chapter jump: the section must not be dropped mid-render.
    RenderLock lock(*this);
    currentSpineIndex = spineIndex;
    nextPageNumber = 0;
    pendingPageJump.reset();
    // An explicit content-offset landing, which outranks every other reposition and survives any
    // difference in pagination -- the same path a bookmark open takes.
    pendingOffsetJump = visibleTextOffset;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::pageflipResumeTo(const PageFlipDecision& decision) {
  // The other user chose, so this device's own prompt has nothing left to ask.
  if (pageflipSyncState == PageFlipSyncState::Resuming) pageflipSetSyncState(PageFlipSyncState::None);

  pageflipSeekToOffset(decision.spineIndex, decision.peerVisibleTextOffset);
  // The role offset is a step, not arithmetic -- a section boundary may sit between the chosen page
  // and this device's -- and it is applied by the pump once the section is back.
  pendingAdvanceForward = decision.forward;
  pendingAdvanceSteps = decision.applyRoleOffset ? 1 : 0;

  snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_RESUME_DONE));
  pageflipSetSyncState(PageFlipSyncState::Reporting);
}

void EpubReaderActivity::pageflipApplyJoin(const PageFlipJoinResolution& resolution, const int ownPage,
                                           const uint32_t ownOffset) {
  switch (resolution.joinCase) {
    case PageFlipJoinCase::Identical:
      // The pair is on one page and owes itself a spread. The right device makes one; the left
      // stays put, so the reader keeps the page it was already looking at.
      //
      // Deliberately not announced. This is a local reposition, not a turn: sending it as one would
      // bump turnSeq and the peer would advance two pages off it, breaking the spread on the very
      // join that created it.
      if (pageflip->getRole() == PageFlipRole::Right) {
        applyAdvance(true, 1);
        requestUpdate();
      }
      break;

    case PageFlipJoinCase::Aligned:
      break;  // the ordinary reopen: the spread is already there, so resume in silence

    case PageFlipJoinCase::Swapped:
      // The user physically swapped the devices, so each takes the other's position and the pair is
      // back in role order. Both devices reach this and both move, which is what makes the exchange
      // a swap rather than one device chasing the other.
      pageflipSeekToOffset(resolution.peerSpineIndex, resolution.peerVisibleTextOffset);
      break;

    case PageFlipJoinCase::Divergent:
      // Section 4.3: the devices were read separately, so neither position is more right than the
      // other and the user picks. Both devices ask, describing their own page; confirming on either
      // makes that device's position the pair's. The interaction is the choice -- there is no list
      // of two positions to read, and it works the same whichever device is in your hand.
      //
      // Nothing seeks yet. A device that guessed here would throw away the position the other user
      // might be about to choose.
      pageflipResumeOwnOffset = ownOffset;
      // Spine and page are one-based for the reader, as everywhere else the reader names a page.
      snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), tr(STR_PAGEFLIP_RESUME_ASK), currentSpineIndex + 1,
               ownPage + 1);
      pageflipSetSyncState(PageFlipSyncState::Resuming);
      break;
  }
}

void EpubReaderActivity::pageflipRefreshCompat() {
  // The viewport is a render() output, so the fingerprint cannot be built in pageflipBegin().
  // Recomputing every pump also covers every later change without needing a hook per setting:
  // orientation, margins, font, line spacing -- and the automatic-page-turn toggle, which moves the
  // bottom margin and so genuinely does re-paginate. That breadth is the point of section 4.4.
  if (!pageflip || buildViewportWidth == 0) return;

  const uint32_t hash = computePageFlipCompatHash(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight),
                                                  pageflipBookId, Section::FILE_VERSION);
  if (hash == pageflipCompatHash) return;

  // The pagination moved, so a page number no longer names the content it did and the cached anchor
  // is about a page that no longer exists. Dropped before anything reads it.
  pageflipCachedOffsetSpine = -1;
  pageflipCachedOffsetPage = -1;

  // Nothing is committed until the position can be read: the greeting and the stored hash have to
  // go out together, so a render in flight defers the whole thing to the next pump. The greeting
  // now carries the join's content anchor too (section 4.2), so it waits on that as well -- a
  // greeting without one would announce a position the peer cannot compare itself against.
  int page = 0;
  uint32_t offset = 0;
  int32_t spineIndex = 0;
  if (!pageflipSettledPosition(spineIndex, page, offset)) return;

  pageflipCompatHash = hash;
  pageflip->setBook(pageflipBookId, hash);
  // Re-greet on every change, first one included: a hello carrying the previous hash is a claim
  // about a layout this device no longer has. The mismatch latch is deliberately NOT cleared here
  // -- it clears when a compatible peer actually answers, so the notice tracks the real state
  // rather than re-firing on every setting the user touches.
  pageflip->announceHello(spineIndex, page, offset);
  LOG_DBG("ERS", "PageFlip layout hash %08X, re-greeting", static_cast<unsigned>(hash));
}

void EpubReaderActivity::pageflipReportMismatch(const PageFlipDecision& decision) {
  // Answer a greeting even from an incompatible peer. The answer carries this device's hash, which
  // is how the other user gets told too -- one device reporting the problem and the other silently
  // doing nothing is a worse outcome than either device alone.
  if (decision.peerWantsReply) pageflipOweJoinDecline = true;

  // Dropping presence is the substance of this phase. A peer that rejects our turns must not gate
  // the two-step advance: leaving it set means this device turns two pages per press while the peer
  // ignores every one of them, which is the silent desync section 5 exists to prevent. Solo reading
  // is wrong-but-usable; that pair would be wrong-and-invisible.
  if (pageflipPeerPresent) {
    pageflipPeerPresent = false;
    LOG_INF("ERS", "PageFlip peer no longer compatible; back to one page per press");
  }
  // Nor does an unusable peer hold this device awake: preventAutoSleep exists for a peer that is
  // driving this one, and this peer cannot.
  if (pageflipCompatMismatch) return;

  pageflipCompatMismatch = true;
  LOG_ERR("ERS", "PageFlip peer has an incompatible layout; not pairing");
  // Acting is immediate; telling the user waits. Two devices rotating together in a shared case do
  // not rotate in the same instant, so the one that turns first genuinely mismatches for a moment
  // before the other catches up. Popping a notice on that transient would train the user to ignore
  // the notice that matters. The latch above prevents re-firing; only this window prevents the
  // first, spurious fire.
  pageflipMismatchSinceMs = millis();
}

void EpubReaderActivity::pageflipReconcileSettings() {
  // Neither of these has a settings-changed hook to hang off -- the web UI writes them straight
  // into SETTINGS under a live reader -- so they are reconciled every pump, exactly as the compat
  // hash is and for the same reason.
  const bool wanted = SETTINGS.pageflipEnabled != 0;
  if (wanted && !pageflip) {
    pageflipBegin();
    return;
  }
  if (!wanted && pageflip) {
    LOG_INF("ERS", "PageFlip turned off; releasing the link");
    pageflipEnd();
    return;
  }
  if (!pageflip) {
    // Paired reading is switched off, so forget that the suspension was announced: switching it
    // back on while WiFi still holds the radio has to say so again rather than fail quietly.
    pageflipWifiSuspended = false;
    return;
  }

  // Role decides which half of the spread this device shows, so a change to it invalidates the
  // classification that produced the current one. Setting it up is the moment a user is most likely
  // to change it, and leaving the session on the old role means both halves believe they are Left:
  // no role offset anywhere, and Identical resolving with neither device stepping.
  const PageFlipRole role = pageflipConfiguredRole();
  if (pageflip->getRole() != role) {
    LOG_INF("ERS", "PageFlip role is now %s; re-negotiating", role == PageFlipRole::Left ? "left" : "right");
    pageflip->setRole(role);
    // Force the greeting that re-runs the join: the hash has not moved, so refreshCompat would not
    // send one on its own.
    pageflipCompatHash = 0;
  }

  // And the paired device, for the same reason and by the same route: the web UI can clear or
  // replace it under a live reader. setPeerMac() re-runs the join itself when the value really
  // changes, because everything the old join concluded was concluded with somebody else.
  pageflipApplyPeerMac();
}

void EpubReaderActivity::pageflipPump() {
  pageflipReconcileSettings();

  // A peer appearing or vanishing changes the status bar, and nothing else in the reader would ever
  // redraw for it -- a page sits until the reader turns it. Without this the badge reports a state
  // that stopped being true minutes ago. Reconciled ahead of the no-link return below, because
  // "there is no link at all" is one of the states it reports: a pair suspended for WiFi (section
  // 6) never gets one, and a pair switched off mid-session has to lose the badge with it.
  if (pageflipBadgeDue() != pageflipBadgeVisible) {
    pageflipBadgeVisible = !pageflipBadgeVisible;
    requestUpdate();
  }

  // An outcome comes off the screen on its own, and it has to do that with no link too: the
  // suspended-for-WiFi notice is posted at exactly the moment there is nothing else to pump.
  if (pageflipSyncState == PageFlipSyncState::Reporting && millis() - pageflipSyncStateSinceMs >= SYNC_REPORT_MS) {
    pageflipSetSyncState(PageFlipSyncState::None);
  }

  if (!pageflip) return;

  // Before anything is sent: the fingerprint has to describe the layout this device is actually
  // using, and this is also where the very first greeting goes out.
  pageflipRefreshCompat();

  // A mismatch that outlived its settling window is a real one, so offer the repair. Driven from
  // here rather than from the receive path because a permanent mismatch produces no further packets
  // to hang the check on -- the peer only re-greets when its own hash changes.
  //
  // The prompt IS the notice (section 5.1): reporting a mismatch with no way to fix it reads as the
  // feature being broken. Both devices reach this point, which is what makes the confirm gesture a
  // choice of source rather than a request that only one user can grant.
  if (pageflipMismatchSinceMs != 0 && millis() - pageflipMismatchSinceMs >= MISMATCH_CONFIRM_MS) {
    pageflipMismatchSinceMs = 0;
    if (pageflipSyncState == PageFlipSyncState::None) pageflipSetSyncState(PageFlipSyncState::Asking);
  }

  // A peer that stopped answering must not leave the exchange up forever. Nothing has been written
  // on either device at this point, which is exactly what makes giving up safe.
  if (pageflipSyncState == PageFlipSyncState::Offering &&
      millis() - pageflipSyncStateSinceMs >= SYNC_ANSWER_TIMEOUT_MS) {
    pageflip->cancelSync();
    snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_SYNC_NO_REPLY));
    pageflipSetSyncState(PageFlipSyncState::Reporting);
  }

  // A link put down for a chapter build comes back here, for the same reason section 6's does: the
  // condition is re-tested every pump rather than hooked to an event, so nothing has to remember to
  // ask. The suspend before it is the backstop for builds that started somewhere other than a
  // chapter crossing, and it is asked first so a stall is never carried for a whole extra pump.
  pageflipSuspendForStalledBuild();
  pageflipResumeAfterColdBuild();

  // Owed steps first: they are what a boundary crossing left behind, and the announce below must
  // report a settled position rather than a half-applied one.
  if (pendingAdvanceSteps > 0 && section && !RenderLock::peek()) {
    consumePendingAdvance();
    requestUpdate();
  }

  //
  // isLinkUp() is part of the wait, not an optimisation. A turn that crosses a chapter boundary can
  // only be announced once the new section is loaded -- the landing page does not exist before that
  // -- and that is exactly the window the link may be down for, laying the chapter out. Announcing
  // into a transport that is down would clear the latch on a send that never happened, and the peer
  // would never hear the press at all: the halves would sit a page apart with nothing to fix it.
  int settledPage = 0;
  int32_t settledSpineIndex = 0;
  if (announceWhenSettled && pendingAdvanceSteps == 0 && section && pageflip->isLinkUp() &&
      pageflipSettledPage(settledSpineIndex, settledPage)) {
    announceWhenSettled = false;
    // Same test loop() uses. The flag tells the peer to show the end panel rather than a page it
    // does not have (section 3, end of book).
    const bool atEnd = settledSpineIndex > 0 && settledSpineIndex >= epub->getSpineItemsCount();
    pageflip->announceLocalTurn(pendingAdvanceForward, settledSpineIndex, settledPage, atEnd);
  }

  // Owed greeting answer. Latched by the receive path below so the answer's position is read under
  // the lock like every other outgoing position, and retried rather than dropped -- a greeting is
  // sent once, so losing the answer leaves the peer waiting indefinitely.
  uint32_t settledOffset = 0;
  if (pageflipOweJoinDecline && pageflip->isLinkUp() &&
      pageflipSettledPosition(settledSpineIndex, settledPage, settledOffset)) {
    pageflipOweJoinDecline = false;
    pageflip->announcePresence(settledSpineIndex, settledPage, settledOffset);
  }

  // The join's answer, for the same reason and with the same retry: it carries a position, and it
  // must not be computed while a render is in flight. Answering also sends this device's reply, so
  // a probe left unanswered would leave the peer waiting -- which is why it stays latched.
  if (pageflipJoinProbePending) pageflipAnswerJoinProbe();

  // Nothing has ever answered, so say hello again. See DISCOVERY_REGREET_MS: the first greeting is
  // the only one this device would otherwise send, and it is sent at the moment two devices started
  // together are least likely to hear each other.
  //
  // repeatHello rather than announceHello, and the difference is not cosmetic: a repeat must not
  // begin a join round. "Nothing has been heard" is read one pump before the queue is drained, so a
  // peer's greeting can already be waiting when this fires -- and two greetings that each restart
  // the other's round make the pair classify twice. That is not theoretical: it put the halves in
  // the swapped arrangement, with the right device showing the page the left was already on.
  if (lastPeerContactMs == 0 && pageflipCompatHash != 0 && pageflip->isLinkUp() &&
      millis() - pageflipLinkUpMs < DISCOVERY_REGREET_WINDOW_MS &&
      millis() - pageflipLastDiscoveryMs >= DISCOVERY_REGREET_MS) {
    int discoveryPage = 0;
    uint32_t discoveryOffset = 0;
    int32_t discoverySpineIndex = 0;
    if (pageflipSettledPosition(discoverySpineIndex, discoveryPage, discoveryOffset)) {
      pageflipLastDiscoveryMs = millis();
      pageflip->repeatHello(discoverySpineIndex, discoveryPage, discoveryOffset);
    }
  }

  // "Still here." It asks for nothing back, so two devices doing this do not talk each other into
  // an ever-growing exchange.
  //
  // Only once a peer has actually been seen this session. A reader with no pair must go on paying
  // nothing for the link -- discovery does not need this, because a device booting second sends a
  // GREETING on its first render and the already-running device answers it. The condition is
  // deliberately "has ever been in contact" and not "is present right now": if a transient outage
  // expired presence on both halves at once, gating on presence would stop both heartbeats and
  // neither could ever re-acquire the other, which is the turnSeq deadlock shape again.
  //
  // The anchor carries the spine index it was read with. This is the one outgoing message that can
  // go out with a render in flight, and render() writes currentSpineIndex -- so the value here has
  // to be the one the anchor was taken from rather than whatever it says at this instant.
  int32_t anchorSpineIndex = 0;
  if (lastPeerContactMs != 0 && millis() - pageflipLastHeartbeatMs >= PEER_HEARTBEAT_MS &&
      pageflipPresenceAnchor(anchorSpineIndex, settledPage, settledOffset)) {
    pageflipLastHeartbeatMs = millis();
    pageflip->announcePresence(anchorSpineIndex, settledPage, settledOffset);
  }

  // And presence expires. Nothing else expires it: a peer that powered off or walked out of range
  // sends no farewell, and leaving the flag set means every press here turns two pages with nobody
  // on the other end showing the second one -- solo reading that silently skips every other page.
  if (pageflipPeerPresent && lastPeerContactMs != 0 && millis() - lastPeerContactMs >= PEER_PRESENCE_TIMEOUT_MS) {
    pageflipPeerPresent = false;
    pageflipPeerWasLost = true;
    LOG_INF("ERS", "PageFlip peer went quiet; back to one page per press");
    snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_PEER_OFFLINE));
    pageflipSetSyncState(PageFlipSyncState::Reporting);
  }

  PageFlipDecision decision;
  if (!pageflip->poll(decision)) return;

  // An incompatible peer is a peer, but not a pair. Handled before contact is recorded below, so it
  // neither holds this device awake nor counts as presence.
  if (decision.action == PageFlipAction::Mismatch) {
    pageflipReportMismatch(decision);
    return;
  }

  // Force-sync traffic (section 5.1) is peer contact -- it keeps both devices awake for as long as
  // the exchange runs -- but it proves nothing about the layout, so it must not set presence or
  // clear the mismatch latch. Both of those follow from the greeting that goes out once an apply has
  // actually changed this device's hash.
  if (decision.action == PageFlipAction::SettingsOffer || decision.action == PageFlipAction::SettingsAnswer ||
      decision.action == PageFlipAction::SettingsApply) {
    lastPeerContactMs = millis();
    if (decision.action == PageFlipAction::SettingsAnswer) {
      pageflipHandleSyncAnswer(decision);
    } else if (decision.settings != nullptr) {
      if (decision.action == PageFlipAction::SettingsOffer) {
        pageflipAnswerOffer(*decision.settings);
      } else {
        pageflipApplySettings(*decision.settings);
      }
    }
    return;
  }

  // Neither device can judge the layout until both have rendered once. Answer the greeting so the
  // handshake completes, but pair on nothing: refreshCompat re-greets with the real hash the moment
  // this device's first render lands, and the peer does the same, so agreement arrives a round
  // later. Pairing here instead would license the two-step advance on a layout nobody has checked.
  if (!decision.layoutDecided) {
    if (decision.peerWantsReply) pageflipOweJoinDecline = true;
    return;
  }

  // Reached only for a decoded packet from a compatible peer, which is what may hold this device
  // awake.
  lastPeerContactMs = millis();
  // Any decoded packet proves a peer, not just a greeting: a turn arriving from a device that
  // booted before this one is equally good evidence.
  if (!pageflipPeerPresent) {
    pageflipPeerPresent = true;
    LOG_INF("ERS", "PageFlip peer present; turns now advance the pair by two");
    // Only worth a notice if the user was told it had gone. Announcing a peer that was never
    // missing would pop a message on every ordinary book open.
    if (pageflipPeerWasLost) {
      pageflipPeerWasLost = false;
      snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_PEER_BACK));
      pageflipSetSyncState(PageFlipSyncState::Reporting);
    }
  }
  // Re-arm the notice: the layouts agree again, so a later divergence is worth reporting afresh.
  // Cancelling an unexpired window here is what makes the rotation case quiet -- the pair
  // reconverges before the notice was ever due.
  if (pageflipCompatMismatch) {
    pageflipCompatMismatch = false;
    pageflipMismatchSinceMs = 0;
    LOG_INF("ERS", "PageFlip peer layout compatible again");
  }

  // The join's question, latched for the pump (section 4.2). Recorded before the switch because it
  // rides the greeting and its answer IS the greeting's reply -- there is no second packet to send,
  // and no case of its own below.
  if (decision.joinProbe) {
    pageflipJoinProbePending = true;
    pageflipJoinRound = decision.joinRound;
    pageflipJoinPeerSpineIndex = decision.spineIndex;
    pageflipJoinPeerOffset = decision.peerVisibleTextOffset;
  }

  switch (decision.action) {
    case PageFlipAction::AdvanceTwo:
      applyAdvance(decision.forward, 2);
      // A step that crossed a section boundary leaves the section unloaded and the rest owed, so
      // report the owed count rather than a page number that does not exist yet.
      LOG_DBG("ERS", "PageFlip peer turn %s -> spine %d page %d, %u owed", decision.forward ? "fwd" : "back",
              currentSpineIndex, section ? section->currentPage : -1, static_cast<unsigned>(pendingAdvanceSteps));
      requestUpdate();
      break;
    case PageFlipAction::Heal:
      pageflipHealTo(decision);
      LOG_DBG("ERS", "PageFlip heal -> spine %d page %d", decision.spineIndex, decision.pageNumber);
      break;
    case PageFlipAction::PeerHello:
      // Nothing to do here: the reply to a compatible peer's greeting is the join's answer, latched
      // above and sent from the pump. A greeting that could not be joined with never reaches this
      // switch -- it took the mismatch or undecided-layout path, which owes a decline instead.
      break;
    case PageFlipAction::JoinResume:
      LOG_INF("ERS", "PageFlip join: reading from the peer's choice, spine %d offset %u", decision.spineIndex,
              static_cast<unsigned>(decision.peerVisibleTextOffset));
      pageflipResumeTo(decision);
      break;
    case PageFlipAction::Mismatch:
      break;  // returned above, before this device counted the peer as present
    case PageFlipAction::SettingsOffer:
    case PageFlipAction::SettingsAnswer:
    case PageFlipAction::SettingsApply:
      break;  // returned above, for the same reason: an exchange is contact, not agreement
    case PageFlipAction::Ignore:
      break;
  }
}

void EpubReaderActivity::pageflipSetSyncState(const PageFlipSyncState state) {
  if (pageflipSyncState == state) return;
  pageflipSyncState = state;
  pageflipSyncStateSinceMs = millis();
  // None is not a notice, it is the absence of one -- but the popup still has to be painted over,
  // so the page is redrawn either way.
  pendingPageflipSyncNotice = state != PageFlipSyncState::None;
  requestUpdate();
}

bool EpubReaderActivity::pageflipHandleSyncInput() {
  const bool confirmed = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool dismissed = mappedInput.wasReleased(MappedInputManager::Button::Back);

  switch (pageflipSyncState) {
    case PageFlipSyncState::Asking:
      if (confirmed) {
        // The interaction is the choice: confirming here says "these are the pair's settings".
        const bool sent =
            pageflip && buildViewportWidth > 0 &&
            pageflip->offerSettings(PageFlipSettingsSync::collect(buildViewportWidth, buildViewportHeight));
        if (sent) {
          pageflipSetSyncState(PageFlipSyncState::Offering);
        } else {
          snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_SYNC_FAILED));
          pageflipSetSyncState(PageFlipSyncState::Reporting);
        }
      } else if (dismissed) {
        pageflipSetSyncState(PageFlipSyncState::None);
      }
      // Held either way: the prompt is a question this device asked, so a press answers it rather
      // than opening the menu or turning a page underneath it.
      return true;

    case PageFlipSyncState::Resuming:
      if (confirmed) {
        // Confirming says "the pair reads from here". This device does not move; the peer seeks to
        // this position and takes its role offset from it.
        if (!pageflip || !pageflip->proposeResume(currentSpineIndex, pageflipResumeOwnOffset)) {
          snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_SYNC_FAILED));
          pageflipSetSyncState(PageFlipSyncState::Reporting);
        } else {
          pageflipSetSyncState(PageFlipSyncState::None);
        }
      } else if (dismissed) {
        // Dismissing leaves both devices where they are. That is a real answer -- the pair goes on
        // reading two unrelated positions -- and the next join re-asks.
        pageflipSetSyncState(PageFlipSyncState::None);
      }
      // Only the two buttons the prompt is actually asking about are held. Unlike a layout
      // mismatch, a divergent join is not a broken pair: both devices are reading correctly, just
      // not together. Holding every button would mean a prompt with no timeout had stopped the
      // reader dead until it was answered -- and this is the ordinary case of one device resumed
      // and the other opened fresh, so it would happen constantly. Turning a page instead answers
      // it by reading on; applyAdvance() takes the prompt down.
      return confirmed || dismissed || mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
             mappedInput.isPressed(MappedInputManager::Button::Back);

    case PageFlipSyncState::Offering:
      return true;  // an exchange is in flight; a page turn now would be applied to a dead layout

    case PageFlipSyncState::Reporting:
      if (confirmed || dismissed) pageflipSetSyncState(PageFlipSyncState::None);
      return true;

    case PageFlipSyncState::None:
      return false;
  }
  return false;
}

void EpubReaderActivity::pageflipAnswerOffer(const PageFlipRenderSettings& offer) {
  // The other user confirmed first, so the choice of source is settled: this device's own prompt
  // goes away rather than inviting a second push back in the other direction.
  if (pageflipSyncState == PageFlipSyncState::Asking) pageflipSetSyncState(PageFlipSyncState::None);

  const PageFlipSettingsSync::Preflight verdict = PageFlipSettingsSync::preflight(
      offer, sdFontSystem.registry(), renderer, automaticPageTurnActive, pageflipBookId);
  LOG_INF("ERS", "PageFlip settings preflight: verdict %u, would hash %08X",
          static_cast<unsigned>(verdict.result), static_cast<unsigned>(verdict.resultHash));
  pageflip->answerOffer(verdict.result, verdict.resultHash);
}

void EpubReaderActivity::pageflipHandleSyncAnswer(const PageFlipDecision& decision) {
  if (decision.syncResult == PageFlipSyncResult::Ok) {
    // Only now does anything get written, and only on the peer -- this device already has these
    // settings. The commit is what turns the preflight into a change.
    pageflip->commitOffer();
    snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_SYNC_DONE));
    pageflipSetSyncState(PageFlipSyncState::Reporting);
    return;
  }

  // An abort has to name the thing to go and fix, and the device to fix it on -- "settings could
  // not be matched" is a statement that something went wrong, not something the user can act on.
  const char* peer =
      decision.peerRole == PageFlipRole::Right ? tr(STR_PAGEFLIP_DEVICE_RIGHT) : tr(STR_PAGEFLIP_DEVICE_LEFT);
  // Both font verdicts print the SD family by name. A built-in font has no name to print, and two
  // built-in fonts only resolve differently across firmware builds -- which is the generic case.
  PageFlipSyncResult reported = decision.syncResult;
  if (SETTINGS.sdFontFamilyName[0] == '\0' &&
      (reported == PageFlipSyncResult::MissingFont || reported == PageFlipSyncResult::FontDiffers)) {
    reported = PageFlipSyncResult::Unknown;
  }
  switch (reported) {
    case PageFlipSyncResult::MissingFont:
      snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), tr(STR_PAGEFLIP_SYNC_MISSING_FONT),
               SETTINGS.sdFontFamilyName, peer);
      break;
    case PageFlipSyncResult::FontDiffers:
      snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), tr(STR_PAGEFLIP_SYNC_FONT_DIFFERS), peer,
               SETTINGS.sdFontFamilyName);
      break;
    case PageFlipSyncResult::ScreenDiffers:
      snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_SYNC_SCREEN_DIFFERS));
      break;
    case PageFlipSyncResult::Ok:
    case PageFlipSyncResult::Unknown:
      snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_SYNC_FAILED));
      break;
  }
  LOG_ERR("ERS", "PageFlip force-sync refused: %s", pageflipSyncMessage);
  pageflipSetSyncState(PageFlipSyncState::Reporting);
}

void EpubReaderActivity::pageflipApplySettings(const PageFlipRenderSettings& offer) {
  if (!PageFlipSettingsSync::apply(offer)) {
    // The pair already agreed on every synced field, so the difference is in something force-sync
    // does not push. Nothing written, no SPIFFS erase cycle, and no rebuild.
    snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_SYNC_SCREEN_DIFFERS));
    pageflipSetSyncState(PageFlipSyncState::Reporting);
    return;
  }

  {
    // Same care as a text-settings change: ensureLoaded() frees the resident SD font that the render
    // task may be walking, and the section must not be dropped mid-render.
    RenderLock lock(*this);
    sdFontSystem.ensureLoaded(renderer);
    if (section) {
      // The page number is about to stop meaning anything -- the layout it counted is being thrown
      // away -- so the position is carried as a content offset and re-derived after the rebuild.
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
  // Outside the lock, like TextSettingsActivity: the SD write must not stall the render task.
  SETTINGS.saveToFile();

  LOG_INF("ERS", "PageFlip applied the pair's settings; rebuilding layout");
  snprintf(pageflipSyncMessage, sizeof(pageflipSyncMessage), "%s", tr(STR_PAGEFLIP_SYNC_DONE));
  // The rebuild's own indexing popup covers the wait; this lands once the page is back.
  pageflipSetSyncState(PageFlipSyncState::Reporting);
}
#endif

bool EpubReaderActivity::skipPages(int amount) {
  if (!section) return false;
  // A chapter skip lands somewhere no arithmetic on the peer's side could predict, so rather than
  // being applied as steps it is announced when this device settles and healed to on the other --
  // the same path a divergent join uses. Dropping the section is a cold build like any other.
  const auto announceAndSuspend = [this](const bool crossedChapter) {
#ifdef FREEINK_CAP_PAGEFLIP
    if (pageflip && pageflipPeerPresent) announceWhenSettled = true;
    if (crossedChapter) pageflipSuspendForColdBuild();
#else
    (void)this;
    (void)crossedChapter;
#endif
  };

  if (amount > 0) {
    {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      section.reset();
    }
    announceAndSuspend(true);
    return true;
  }
  if (section->currentPage > 0) {
    section->currentPage = 0;
    announceAndSuspend(false);
    return true;
  }
  if (currentSpineIndex > 0) {
    {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      section.reset();
    }
    announceAndSuspend(true);
    return true;
  }
  return false;
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

bool EpubReaderActivity::skipLoopDelay() {
  return section && section->isBuilding() && !buildHeapPaused &&
         (section->isPartial() || pageflipBuildingForSuspendedLink() ||
          static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
}

void EpubReaderActivity::renderBook() {
  currentPageLinks.clear();
  if (!epub) return;

  // One-shot notices raised outside renderBook(), shown after the page they belong to has been
  // drawn. Sharing one exit hook keeps two notices from drawing over each other in the same frame.
  const auto showPendingNotice = [this]() {
    if (pendingSyncSaveError) {
      pendingSyncSaveError = false;
      GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
      return;
    }
#ifdef FREEINK_CAP_PAGEFLIP
    if (pendingPageflipSyncNotice) {
      pendingPageflipSyncNotice = false;
      // Asking and Reporting are the only states with something to say. Offering is a wait, and the
      // wait that matters -- the layout rebuild after an apply -- already has the indexing popup.
      if (pageflipSyncState == PageFlipSyncState::Asking) {
        GUI.drawPopup(renderer, tr(STR_PAGEFLIP_SYNC_ASK));
      } else if (pageflipSyncState == PageFlipSyncState::Resuming ||
                 pageflipSyncState == PageFlipSyncState::Reporting) {
        // Both carry a sentence built for the occasion: the resume prompt names this device's own
        // page, which is the whole basis on which the user is choosing between the two.
        GUI.drawPopup(renderer, pageflipSyncMessage);
      }
    }
#endif
  };

  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;
  }

  // Shared with PageFlip's settings preflight, which asks the same question about a margin this
  // device has not adopted yet (docs/pageflip.md section 5.1).
  const ReaderUtils::ReaderLayoutBox layoutBox =
      ReaderUtils::readerLayoutBox(renderer, SETTINGS.screenMargin, automaticPageTurnActive);
  const int orientedMarginTop = layoutBox.marginTop;
  const int orientedMarginRight = layoutBox.marginRight;
  const int orientedMarginBottom = layoutBox.marginBottom;
  const int orientedMarginLeft = layoutBox.marginLeft;
  const uint16_t viewportWidth = layoutBox.viewportWidth;
  const uint16_t viewportHeight = layoutBox.viewportHeight;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    partialRebuildStartFailed = false;

    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();
          showBuildError();
          return;
        }
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingNotice();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingNotice();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingNotice();
        return;
      }
      requestUpdate();
      showPendingNotice();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    currentPageFootnotes = std::move(p->footnotes);
    currentPageLinks = std::move(p->links);
    currentPageLinkMarginLeft = orientedMarginLeft;
    currentPageLinkMarginTop = orientedMarginTop;

    // The overlay and non-tiled grayscale renderer share the renderer's single
    // stored-BW slot. Release the old page snapshot before renderContents()
    // needs that slot, then snapshot the newly rendered page below.
    discardOverlayPage();

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
  }

  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingNotice();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // FAST, same as openOverlay: HALF's inverting pass flashes the sheet
    // (white, in night mode) on every repaint under an open panel. Any AA
    // residue a FAST differential leaves under the chrome has not shown in
    // practice; restore a HALF cleanup here if text ever visibly ghosts
    // through the sheet (see #2190 for the mechanism).
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  std::optional<uint32_t> offset;
  if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  // Scan the status bar too: a CJK book/chapter title redirected to the SD
  // fallback font joins the page's single batch prewarm instead of triggering
  // its own SD pass after the scope ends.
  renderStatusBar();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase = tiledGrayscale && !pageHasImages && renderer.combinesGrayscaleBase();
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Image pages use one base refresh before the grayscale pass. FAST leaves
    // the panel receptive to the gray waveform; pending cleanup still honors
    // the scheduled/manual HALF refresh.
    renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh = 1;
  } else if (combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  const auto tDisplay = millis();

  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section ? section->currentPage + 1 : 1;
  const float pageCount = section ? section->estimatedTotalPages() : 1;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    if (epub) {
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        const auto tocItem = epub->getTocItem(tocIndex);
        title = tocItem.title;
      }
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

  // The badge marks a pair that is configured but not connected -- the one state the reader cannot
  // show by itself. A working pair needs no badge: the other device is right there showing the next
  // page. See BaseTheme::PairStatus.
  auto pairStatus = BaseTheme::PairStatus::None;
#ifdef FREEINK_CAP_PAGEFLIP
  if (pageflipBadgeDue()) {
    pairStatus = pageflipConfiguredRole() == PageFlipRole::Left ? BaseTheme::PairStatus::OfflineLeft
                                                                : BaseTheme::PairStatus::OfflineRight;
  }
#endif
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false, pairStatus);
}

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
constexpr StrId kTextRowNames[] = {StrId::STR_FONT, StrId::STR_FONT_SIZE, StrId::STR_LINE_SPACING,
                                   StrId::STR_PARA_ALIGNMENT, StrId::STR_FOCUS_READING};
constexpr StrId kSpacingIds[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId kAlignIds[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                               StrId::STR_BOOK_S_STYLE};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kSpacingIds) == CrossPointSettings::LINE_COMPRESSION_COUNT, "line spacing labels");
static_assert(std::size(kAlignIds) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  // Touch-first chrome: button boards always get the classic list menu, even
  // if a settings file (e.g. an SD card moved from a touch board) says Toolbar.
  return mappedInput.hasTouch() && SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) {
    return epub->getTocItem(tocIndex).title;
  }
  return tr(STR_UNNAMED);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_NOTO_SERIF, StrId::STR_NOTO_SANS};
  switch (row) {
    case 0:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[SETTINGS.fontFamily % CrossPointSettings::FONT_FAMILY_COUNT]);
    case 1:
      return std::to_string(SETTINGS.fontPointSize) + " pt";
    case 2:
      return I18N.get(kSpacingIds[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
    case 3:
      return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
    case 4:
      return SETTINGS.focusReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// Settings-style option pickers for the Text panel's enum rows. Every
// selection applies immediately to the page under the sheet.
void EpubReaderActivity::showTextRowPopup(const int row) {
  switch (row) {
    case 1: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        SETTINGS.fontPointSize = sizes[idx];
        applyTextSettingLive();
      });
      break;
    }
    case 2:
      overlayPopup.show(StrId::STR_LINE_SPACING, kSpacingIds, static_cast<int>(std::size(kSpacingIds)),
                        SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case 3:
      overlayPopup.show(StrId::STR_PARA_ALIGNMENT, kAlignIds, static_cast<int>(std::size(kAlignIds)),
                        SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, [this](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

void EpubReaderActivity::openOverlay(Overlay target) {
  const Overlay previous = overlay;
  overlay = target;
  if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
  if (previous == Overlay::None) toolbarUi->begin();
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents:
      panelIndex = std::max(0, epub->getTocIndexForSpineIndex(currentSpineIndex));
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    case Overlay::Text:
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  //
  // Refresh mode: FAST for every overlay paint, first open included. The AA
  // pass only grays glyph edges, and residue a FAST differential leaves under
  // the sheet has not shown in practice; it also self-heals on the
  // Xteink-class panels, whose close path re-renders the page. If text or
  // images ever visibly ghost through the chrome, restore a HALF cleanup on
  // the first open (see #2190 for the mechanism).
  if (section) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    RenderLock lock;
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    requestUpdate();  // no page yet: renderBook() draws the overlay once it is
  }
}

// Close the overlay back to the reading page. Boards without the Xteink
// grayscale-AA pass restore the page snapshot and push one FAST refresh -- no
// re-render, no flash; Xteink boards re-render to restore the AA planes.
void EpubReaderActivity::closeOverlayToPage() {
  overlay = Overlay::None;
  overlayPopup.dismiss();  // an option picker cannot outlive its panel
  toolbarUi.reset();       // ~1 KB of interaction table + props, only needed while open
  if (!xteinkClassPanel() && overlayPageStored) {
    RenderLock lock;  // the render task shares the framebuffer
    // No baseline resync: the glass shows the chrome, and erasing it needs
    // the differential to keep diffing against the last pushed frame.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  discardOverlayPage();
  requestUpdate();  // redraw the clean page
}

void EpubReaderActivity::renderOverlay() {
  if (!epub || !section || !toolbarUi) return;

  ReaderToolbarUi::Model model;
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    const int pageCount = section->estimatedTotalPages();
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(section->currentPage + 1) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
    pageInfo = std::to_string(section->currentPage + 1) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    return;
  }

  // Panels (Contents / Text / More): a bottom sheet over the page + button hints.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.bottomReserve = UITheme::getInstance().getMetrics().buttonHintsHeight;
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    model.itemCount = epub->getTocItemsCount();
    model.rowText = [this](int i) {
      const auto item = epub->getTocItem(i);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      return std::string(depth, ' ') + item.title;
    };
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    renderOverlay();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  };

  // Jump to another spine item (chapter scrub). The overlay stays up and is
  // re-drawn over the new page by renderBook().
  const auto gotoSpine = [this](int target) {
    const int spineCount = epub->getSpineItemsCount();
    target = std::clamp(target, 0, spineCount - 1);
    if (target != currentSpineIndex) {
      RenderLock lock;
      clearDeferredReposition();
      nextPageNumber = 0;
      currentSpineIndex = target;
      section.reset();
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    return tool == 0 ? Overlay::Contents : (tool == 1 ? Overlay::Text : Overlay::More);
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoSpine(currentSpineIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoSpine(currentSpineIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoSpine(static_cast<int>((static_cast<float>(routed.permille) / 1000.0f) *
                                       static_cast<float>(epub->getSpineItemsCount() - 1) +
                                   0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + 2) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % 3;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoSpine(currentSpineIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents ? epub->getTocItemsCount()
                    : overlay == Overlay::Text   ? kTextRowCount
                                                 : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == 0) {
        // Full font picker (built-in + SD fonts, live preview) -- the same
        // screen Settings uses; a popup cannot scroll a long font list.
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                      TextSettingsActivity::Tab::Family),
                               [this](const ActivityResult&) {
                                 applyReaderTextSettings();
                                 overlay = Overlay::Text;  // back to the Text panel
                                 panelIndex = 0;
                                 if (toolbarUi) toolbarUi->begin();  // the picker drew its own FUI screen
                                 requestUpdate();                    // re-render page + Text panel
                               });
      } else if (panelIndex == 4) {
        // Focus Reading is a genuine on/off: a tap toggles and applies live.
        SETTINGS.focusReadingEnabled = SETTINGS.focusReadingEnabled ? 0 : 1;
        applyTextSettingLive();
      } else {
        // Enum rows open the Settings-style option picker.
        showTextRowPopup(panelIndex);
      }
    } else if (overlay == Overlay::Contents) {
      const auto item = epub->getTocItem(panelIndex);
      if (item.spineIndex != -1) {
        RenderLock lock;
        clearDeferredReposition();
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
        nextPageNumber = 0;
        section.reset();
      }
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list. Checked before the routed-frame return:
  // FUI routes every touch frame over the sheet, so a swipe's frames count as
  // routed (without dispatching -- too much travel for a tap) and the gesture
  // would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismissPanel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  overlayPopup.render(renderer);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void EpubReaderActivity::applyReaderTextSettings() {
  SETTINGS.saveToFile();
  // (Re)load or unload the selected SD-card font for the current family/size.
  // The reader otherwise only loads SD fonts on book open, so without this an
  // in-reader font change wouldn't take effect until re-opening the book.
  sdFontSystem.ensureLoaded(renderer);
  RenderLock lock;
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();  // force re-pagination with the new settings
}

// The More panel carries everything the classic list menu offers except the
// two entries that have their own tool (chapters -> Contents, text -> Text).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
  EpubReaderMenuActivity::buildMenuItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty());
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS;
                                 }),
                  moreItems.end());
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    case MA::NIGHT_MODE:
      return SETTINGS.screenInverted ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::FRONTLIGHT:
      return Frontlight.isOn() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  const auto action = moreItems[row].action;
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(idx));
      });
      paintOverlayPopup();
      return;
    }
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  discardOverlayPage();
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark();
    showBookmarkMessage = true;
    bookmarkMessageTime = millis();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  // Actions that neither open a screen nor leave the reader (a sync with no
  // credentials, say) would otherwise leave the closed panel on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;
    return;
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
