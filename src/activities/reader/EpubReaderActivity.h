#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#ifdef FREEINK_CAP_PAGEFLIP
#include <PageFlipCompat.h>
#include <PageFlipSession.h>
#include <PageFlipTransportFactory.h>

#include "PageFlipSettingsSync.h"
#endif

#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  // Image pages use a dedicated double-FAST refresh path, so retain a manual
  // refresh request until renderContents can issue its clean base pass.
  bool forcedRefreshPending = false;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  // Visible-codepoint offset of the page currently on screen, captured when the page is loaded
  // (Page::visibleTextOffset). Lets saveProgress persist the offset without reopening section.bin.
  std::optional<uint32_t> currentPageVisibleOffset;
  // Explicit "land at this visible-codepoint offset in the target spine" request (bookmark open).
  // Resolved in render() once the section is loaded/built far enough, then cleared. Unlike a
  // settings-change reposition it always resolves by content, so it survives any re-pagination.
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
#ifdef FREEINK_CAP_PAGEFLIP
  // Paired reading: this device and its peer are one two-page spread, so a turn advances each of
  // them by two (docs/pageflip.md section 3). Declared before the session, which holds a reference
  // to it -- members are destroyed in reverse order, so the session goes first.
  std::unique_ptr<PageFlipTransport> pageflipTransport;
  std::unique_ptr<PageFlipSession> pageflip;
  // Steps still owed to the advance in progress. A step that crosses a section boundary unloads
  // the section, and the landing page is unknown until render() has loaded the neighbouring one,
  // so the remainder waits rather than dereferencing a section that is not there.
  uint8_t pendingAdvanceSteps = 0;
  bool pendingAdvanceForward = true;
  // Broadcast the local turn only once its steps have settled, so the position on the wire is the
  // page actually being shown rather than a half-applied one.
  bool announceWhenSettled = false;
  unsigned long lastPeerContactMs = 0;
  // A started link is not a peer. Until one answers, this device reads exactly as it does today --
  // one page per press -- because a lone device advancing by two would turn two pages on every
  // press, which is the whole feature going wrong in the most visible way possible.
  bool pageflipPeerPresent = false;
  // The path hash the rest of the reader identifies this book by, kept so the compatibility hash
  // can be rebuilt whenever the layout changes without reopening the epub.
  uint32_t pageflipBookId = 0;
  // Layout fingerprint (docs/pageflip.md section 5). Zero means "not computed yet": the viewport is
  // a render() output, so it is unknown until the first page has been laid out.
  uint32_t pageflipCompatHash = 0;
  // Latched so the notice fires on the transition into mismatch rather than on every packet, and
  // so a later change back into agreement can re-arm it.
  bool pageflipCompatMismatch = false;
  // Consumed by render(), which owns the screen. The pump runs on the main task and must not draw.
  bool pendingPageflipMismatch = false;
  // When the current mismatch was first seen, 0 when none is outstanding. The notice waits out this
  // window so a pair mid-rotation -- briefly disagreeing because one device turned first -- does not
  // pop one. Dropping the pairing is NOT delayed; only telling the user is.
  unsigned long pageflipMismatchSinceMs = 0;
  static constexpr unsigned long MISMATCH_CONFIRM_MS = 4000;
  // A greeting from a peer this device cannot join with -- an incompatible layout, or one neither
  // side has decided yet. It still gets an answer, because the answer carries this device's own
  // hash and that is how the other user gets told; latched rather than sent inline because the
  // answer carries a position, which cannot be read while a render is in flight.
  bool pageflipOweJoinDecline = false;

  // The join negotiation (docs/pageflip.md section 4.2). A probe arrives on the receive path and is
  // answered from the pump, for the same reason: the verdict and this device's own position both
  // have to be read with no render in flight, and they have to be read together or the pair is
  // classified from two different moments.
  bool pageflipJoinProbePending = false;
  uint32_t pageflipJoinRound = 0;
  int32_t pageflipJoinPeerSpineIndex = 0;
  uint32_t pageflipJoinPeerOffset = 0;
  // A device being read from but not pressed sees no input of its own, so peer traffic has to keep
  // it awake. Only decoded packets from a compatible peer count -- see the receive path.
  static constexpr unsigned long PEER_ACTIVITY_WINDOW_MS = 3000;

  // The settings force-sync (docs/pageflip.md section 5.1). Detecting a mismatch without a way to
  // fix it reads as "the feature is broken", so a confirmed mismatch offers the repair.
  //
  // The gesture is section 4.3's: both devices ask, and confirming on one makes THAT device the
  // source. The interaction is the choice -- there is no list of two positions to read, and it
  // works the same whichever device you happen to be holding.
  enum class PageFlipSyncState : uint8_t {
    None,
    Asking,     // prompt up on this device: Confirm pushes ours, Back dismisses
    Offering,   // our settings are out, waiting on the peer's preflight
    Applying,   // the peer's settings are being written and the layout rebuilt here
    Reporting,  // an outcome is on screen: matched, or why it could not be
  };
  PageFlipSyncState pageflipSyncState = PageFlipSyncState::None;
  // Set when the state changes, so render() draws the new state once rather than every frame.
  bool pendingPageflipSyncNotice = false;
  // The outcome text, built once when the answer lands: it names the font and the device, which is
  // what makes it something the user can act on rather than a statement that something went wrong.
  char pageflipSyncMessage[96] = "";
  unsigned long pageflipSyncStateSinceMs = 0;
  // A peer that stops answering must not leave the prompt up forever -- it powered off, or walked
  // out of range, and either way nothing has been written on either device.
  static constexpr unsigned long SYNC_ANSWER_TIMEOUT_MS = 6000;
  // How long an outcome stays on screen before the page comes back.
  static constexpr unsigned long SYNC_REPORT_MS = 4000;
#endif
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  // "No dictionary set" popup, shown when a lookup is triggered without a configured dictionary.
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  // Idle-time glyph prewarm: after a page settles, scan the LIKELY next page
  // (scan mode draws nothing) and load its missing glyphs from SD during idle,
  // so the next turn's in-render prewarm is a cache hit instead of ~100 ms of
  // SD reads on the page-turn critical path. One attempt per position.
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick; the blocking extension in render() remains the fallback past the watermark.
  bool partialRebuildStartFailed = false;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;

  // MEMFIX-PORT: background-build heap floor; portable
  // Skip background build ticks below this free-heap floor. The parse path grows
  // word vectors of heap strings — throwing allocations that abort() on OOM under
  // -fno-exceptions (field crash: bad_alloc in ParsedText::addWord during a
  // background tick under heap pressure). The tick is deferrable work:
  // page-turn transients free up between turns and the build resumes; the render
  // path still builds the page it actually needs regardless of this floor.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  // Fragmentation floor for the same gate: a tick passed the free-heap floor at
  // 34.7 KB free but the largest block was ~11 KB, and a parse allocation inside the
  // tick aborted anyway. Free heap says how much memory exists; maxAlloc says whether
  // any single allocation can actually have it. 16 KB also keeps the advance-table
  // batch path (16 KB scratch) viable during builds.
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Gate for a background build tick: true when the heap can take parse allocations.
  // Updates buildHeapPaused as a side effect.
  bool buildTickHeapGate();
  // True while the background build is gated on the heap floors. Lets skipLoopDelay()
  // return the loop to normal delay/power-saving during the pause: isBuilding() stays
  // true the whole time, and without this the loop would spin at full CPU speed doing
  // no build work — indefinitely, if the build context itself keeps the heap low.
  bool buildHeapPaused = false;
  // Heap floor for optional render-adjacent work (idle prewarm). Page
  // deserialization (TextBlock word vectors/strings) and glyph caching allocate
  // through throwing paths that abort() on OOM; skip deferrable work below it.
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Deadline backstop for the predictive gates above: if the blocking build-to-target still
  // hasn't produced the landing page this long after the build started, surface the popup
  // mid-build. Builds that finish under the deadline stay popup-free.
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  // True only during onEnter's blocking build-to-target phase, until the popup has been
  // drawn. Gates showBuildPopup() so the parser's popup callback (which persists into
  // background buildSomeMore chunks) can never draw over a displayed page.
  bool buildPopupPending = false;
  // Draw the indexing popup mid-build (parser image-probe callback and deadline backstop).
  void showBuildPopup();
  // Map the cached content position into the rebuilt section (used after a
  // settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void openDictionaryWordSelect();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  // Applies a single page step to the current position. Requires a loaded `section`, and takes
  // the RenderLock itself when the step crosses a section boundary -- so it must not be called
  // from render(), which holds that (non-recursive) lock for its whole body.
  // Returns false when the step crossed a section boundary: the section is unloaded and the
  // landing page is only known once render() has loaded the neighbouring one, so a caller
  // stepping more than once must defer the rest rather than call again immediately.
  bool advanceOnePage(bool isForwardTurn);
  void pageTurn(bool isForwardTurn);
#ifdef FREEINK_CAP_PAGEFLIP
  // Brings the pair link up for this book, or leaves the reader solo if it cannot start. Reading
  // never blocks on the peer.
  void pageflipBegin();
  void pageflipEnd();
  // Pumped once per frame: drains owed steps, announces a settled local turn, applies one peer
  // packet.
  void pageflipPump();
  // Queues `steps` single-page steps and applies as many as can be applied right now.
  void applyAdvance(bool forward, uint8_t steps);
  void consumePendingAdvance();
  // Seeks to the peer's absolute position, then owes one further step when the roles differ -- the
  // pair is one page apart and that page is only knowable by stepping, not by arithmetic.
  void pageflipHealTo(const PageFlipDecision& decision);
  // Rebuilds the layout fingerprint from the viewport the last render actually used, and re-greets
  // the peer when it changes. Called every pump because there is no single "settings changed" hook
  // covering everything that moves the viewport.
  void pageflipRefreshCompat();
  // The page this device is actually showing, for the position field of an outgoing packet.
  // Returns false while a render is in flight: `section` belongs to the render task, which assigns
  // and resets it under the lock, so reading it unguarded is a use-after-free. Callers retry on the
  // next pump rather than block the main task behind a full page render.
  bool pageflipSettledPage(int& page);
  // The same, plus the content offset the join negotiation compares (section 4.2). Separate because
  // it can fail where the page alone cannot: a section that is not loaded, or a page outside it,
  // has no anchor to report, and a greeting without one announces a position the peer cannot test
  // itself against. Turns keep using the page-only form -- they run between devices already proven
  // to share a layout, where a page number means the same thing on both.
  bool pageflipSettledPosition(int& page, uint32_t& visibleTextOffset);
  // Drops back to solo reading and arms the notice. A peer that cannot apply our turns must not
  // gate the two-step advance.
  void pageflipReportMismatch(const PageFlipDecision& decision);
  // This device's whole contribution to the join (section 4.2), taken under one lock: where it is,
  // and whether its next page starts where the peer says it is. Returns false when a render is in
  // flight or the section cannot answer, in which case the probe stays latched for the next pump.
  bool pageflipJoinAnswerInputs(int32_t peerSpineIndex, uint32_t peerVisibleTextOffset, int& page,
                                uint32_t& visibleTextOffset, PageFlipJoinVerdict& verdict);
  // Answers a latched probe and acts on the classification if it completed one.
  void pageflipAnswerJoinProbe();
  void pageflipApplyJoin(const PageFlipJoinResolution& resolution);
  // Lands on a content offset in another spine, the one navigation that survives any difference in
  // pagination. Used for the cases where this device has to move to where the peer is.
  void pageflipSeekToOffset(int32_t spineIndex, uint32_t visibleTextOffset);
  // Force-sync (section 5.1). The prompt owns Confirm and Back while it is up; returns true when it
  // consumed the input, so the reader menu does not also open behind it.
  bool pageflipHandleSyncInput();
  void pageflipSetSyncState(PageFlipSyncState state);
  // Preflights a peer's offer against this device and answers it. Writes nothing.
  void pageflipAnswerOffer(const PageFlipRenderSettings& offer);
  // Commits on Ok, and on anything else builds the sentence that says which device needs what.
  void pageflipHandleSyncAnswer(const PageFlipDecision& decision);
  // Writes the peer's settings, reloads fonts and rebuilds the layout, re-anchored on the content
  // offset -- the saved page number belongs to a layout that no longer exists.
  void pageflipApplySettings(const PageFlipRenderSettings& offer);
#endif
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              int initialRefreshCountdown)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed + fast loop ticks while a section build runs: at the low-power
  // frequency a giant chapter's background rebuild stretches from ~40s to many
  // minutes, so the reader exits before it can finalize and the next open restarts
  // it from page 0. Reverts to normal power behavior the moment the build finishes,
  // and while the build is heap-paused (no work is happening, so spinning at full
  // speed would only burn battery; the paused gate still retries every loop pass).
  bool skipLoopDelay() override { return section && section->isBuilding() && !buildHeapPaused; }
  bool isReaderActivity() const override { return true; }
#ifdef FREEINK_CAP_PAGEFLIP
  // The peer's traffic is this device's activity: it is being read from, just not pressed, and
  // today's inactivity timer would otherwise sleep it out from under the reader.
  bool preventAutoSleep() override {
    return pageflip && lastPeerContactMs != 0 && millis() - lastPeerContactMs < PEER_ACTIVITY_WINDOW_MS;
  }
#endif
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
      forcedRefreshPending = true;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
