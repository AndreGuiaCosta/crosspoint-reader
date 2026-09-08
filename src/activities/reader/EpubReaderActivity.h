#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#ifdef FREEINK_CAP_PAGEFLIP
#include <PageFlipCompat.h>
#include <PageFlipMac.h>
#include <PageFlipSession.h>
#include <PageFlipTransportFactory.h>

#include "PageFlipSettingsSync.h"
#endif

#include "BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "components/OptionPopup.h"

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
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
  // This device's own anchor, captured when the divergent prompt went up, so confirming proposes
  // the position the prompt actually describes rather than wherever the reader has drifted to.
  uint32_t pageflipResumeOwnOffset = 0;
  // A device being read from but not pressed sees no input of its own, so peer traffic has to keep
  // it awake. Only decoded packets from a compatible peer count -- see the receive path.
  static constexpr unsigned long PEER_ACTIVITY_WINDOW_MS = 3000;
  // How often a device says "still here" when nothing else is being sent. Presence is what licenses
  // the two-step advance, and until this existed the only evidence of a peer was a page turn -- so a
  // pair being read on one device had no traffic at all between presses, and the window above could
  // never be satisfied. Section 7's duty cycling will want to revisit the rate; the constraint it
  // must respect is that it stays comfortably inside PEER_ACTIVITY_WINDOW_MS.
  static constexpr unsigned long PEER_HEARTBEAT_MS = 2000;
  // Four missed heartbeats. A peer that powered off or walked out of range says nothing on its way
  // out, and nothing else in the protocol would ever notice: this device would go on turning two
  // pages per press with no second half to show the other one -- the whole feature failing in the
  // most visible way there is.
  static constexpr unsigned long PEER_PRESENCE_TIMEOUT_MS = PEER_HEARTBEAT_MS * 4;
  unsigned long pageflipLastHeartbeatMs = 0;
  // Discovery is one packet, and one packet is not a protocol. The greeting goes out when the
  // layout fingerprint first exists and never again unless it moves, so two devices switched on
  // together -- each greeting before the other's radio is listening -- never meet, and a single
  // dropped datagram has exactly the same permanent effect on a real radio. So it is repeated,
  // slowly, while nothing has EVER been heard from a peer.
  //
  // Bounded, because a peer switched on later is found by ITS greeting, which this device is
  // listening for: the window only has to cover devices starting at about the same time. Outside it
  // a solo reader with paired reading left on goes back to costing nothing, which is the property
  // the heartbeat's "has ever been in contact" gate exists to protect.
  static constexpr unsigned long DISCOVERY_REGREET_MS = 3000;
  static constexpr unsigned long DISCOVERY_REGREET_WINDOW_MS = 30000;
  unsigned long pageflipLastDiscoveryMs = 0;
  // When the link came up, so the status badge can hold its tongue while a peer is still answering.
  // Without the grace period the badge draws on the very first render -- a greeting takes a moment
  // to come back -- and then has to be un-drawn, costing a second full refresh at every book open.
  // The window is the same one presence expires on: the badge appears exactly when this device
  // would have given a peer up for gone.
  unsigned long pageflipLinkUpMs = 0;
  // What the badge showed at the last render. The reader has no reason of its own to redraw when a
  // peer appears or vanishes, so without this the badge would be stale until the next page turn --
  // reporting a peer as offline for as long as the reader sat still.
  bool pageflipBadgeVisible = false;
  // Whether the user was told the peer had gone, so the pair coming back is worth a notice. Without
  // it, "paired device is back" would fire on every ordinary book open.
  bool pageflipPeerWasLost = false;
  // Whether the link is being held down because WiFi has the radio (docs/pageflip.md section 6).
  // Latched only so the notice is said once: the condition itself is re-tested every pump, which is
  // what brings the link back when WiFi goes away.
  bool pageflipWifiSuspended = false;
  // The last anchor looked up, so a heartbeat for a page the reader has not left costs nothing.
  // Resolving an offset reads the section file once the chapter has finalized, and at the heartbeat
  // rate that would be constant SD traffic for an answer that cannot have changed.
  int pageflipCachedOffsetSpine = -1;
  int pageflipCachedOffsetPage = -1;
  uint32_t pageflipCachedOffset = 0;
  // The last anchor actually read, so saying "still here" never has to wait for the render lock.
  //
  // Every other outgoing position is deliberately skipped while a render is in flight -- it would
  // otherwise describe a page this device is in the middle of leaving. Liveness is the exception,
  // and treating it like the rest is what made a busy device look like a dead one: a render on real
  // e-ink is seconds (4,241 ms measured on an image-bearing page against 0.88 s for a text page),
  // two of them back to back outlast PEER_PRESENCE_TIMEOUT_MS, and the peer then withdraws presence
  // from a device that is running perfectly well. The next press there advances one page instead of
  // two, which desyncs the spread -- so the cost is a wrong page, not a wrong notice.
  //
  // Kept separate from the offset cache above, which is dropped whenever the pagination moves: a
  // stale page number is exactly what this must still be able to answer with, because the claim it
  // makes is "this device is awake", not "this device is here".
  // Free heap under which the link is put down for the length of a cold chapter build.
  //
  // A build is the reader's largest transient, and the radio is 60 KB of resident heap it does not
  // need while one runs. With the link up a device sat at 40,728 B free; the same device building a
  // cold section bottomed out at 4,384 B, and its partner -- building the SAME chapter at the same
  // moment, because a paired turn advances both halves at once -- ran out and called abort() from a
  // throwing STL allocation inside the builder. The pair does not merely share the exposure, it
  // doubles it, and the code that dies is the code that cannot check first.
  //
  // The number is a first estimate and wants a bench pass: it is set above the ~40 KB a device has
  // with the link up, so a cold crossing always releases, and comfortably under what the same
  // device has without it, so the link comes back afterwards.
  static constexpr uint32_t PAGEFLIP_COLD_BUILD_MIN_FREE_HEAP = 64 * 1024;
  // The floor for coming BACK, which is not the floor for standing down and must never be.
  //
  // The original code used one number for both, reasoning that a suspension only starts at a cold
  // crossing so the resume could not walk into its own trigger. Measured on two X4s 2026-09-05, it
  // walks straight into it: after the build the device idles at 47,296 B with the link DOWN, under
  // the 64 KB above, so the resume was blocked forever and the pair silently fell back to solo for
  // the rest of the session. The same run also showed why the shared number was never going to
  // work -- releasing the transport gave back nothing at all (52,212 B free with the link up,
  // 47,296 B with it down), so a device that has stood down does not have more room than one that
  // has not, it has slightly less.
  //
  // This is therefore an OOM guard and nothing more: comfortably under the ~47 KB a device actually
  // has at this point, comfortably above nothing. begin() failing is handled -- the latch is kept
  // and the next pump asks again -- so this only has to catch the case where trying is reckless.
  static constexpr uint32_t PAGEFLIP_COLD_BUILD_RESUME_MIN_FREE_HEAP = 24 * 1024;
  // Throttle for the one-line "why is the link still down" diagnostic below.
  static constexpr unsigned long PAGEFLIP_COLD_BUILD_WAIT_LOG_MS = 5000;
  // How long a suspended link waits for a section that never appears before giving up on it.
  //
  // A build that fails resets the section and reports nothing else, so "null section" is the only
  // trace it leaves -- and that is also what the moments before render() constructs the new one look
  // like. This separates them: past this, a chapter that still is not there is not coming.
  static constexpr unsigned long PAGEFLIP_COLD_BUILD_START_GRACE_MS = 5000;
  // How long the pair stays down for one chapter before the build is put down instead of waited on.
  //
  // The link now waits for the build to be genuinely over rather than merely far enough ahead,
  // because handing the radio back to a live build deadlocks the device (see pageflipColdBuildOver).
  // But BUILD_WINDOW_AHEAD's comment states the unbounded case as a design fact -- a giant
  // single-spine book never finalizes in one sitting -- so an unconditional wait would cost the pair
  // the rest of that book. Sized well past an ordinary cold chapter (a 14-page one measured ~15 s on
  // an X4) so it is the giant-spine escape hatch and not a second path an ordinary crossing takes.
  //
  // Being generous is also what stops the escape hatch from flapping. The release persists a partial
  // and loop() restarts a partial's extension once the reader is within PARTIAL_REBUILD_START_MARGIN
  // (15 pages) of its watermark -- which would suspend the link again. At ~100-300 ms per page this
  // long a build leaves a watermark hundreds of pages ahead of the reader, far outside that margin,
  // so the restart does not fire until the reader has actually read most of the way there.
  static constexpr unsigned long PAGEFLIP_COLD_BUILD_DEADLINE_MS = 45000;
  // Whether the link is down for a build rather than for WiFi (section 6) or by choice. Kept apart
  // from pageflipWifiSuspended precisely so the two resume paths cannot be confused: they wait on
  // different things and one must not clear the other's latch.
  bool pageflipHeapSuspended = false;
  // The chapter the suspension above is being held for, and when it started. Latched at the suspend
  // because that is the only place the answer exists: by the time the resume runs, the section has
  // been dropped and the reader's own state cannot say which build is being waited on.
  int32_t pageflipColdBuildSpine = -1;
  unsigned long pageflipColdBuildStartMs = 0;
  unsigned long pageflipColdBuildWaitLogMs = 0;
  bool pageflipHaveLastAnchor = false;
  int32_t pageflipLastAnchorSpine = 0;
  int pageflipLastAnchorPage = 0;
  uint32_t pageflipLastAnchorOffset = 0;

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
    // Section 4.3's divergent join, which is the same gesture over a different subject: both
    // devices ask, and confirming on one makes THAT device's position the pair's. Sharing the
    // state machine rather than growing a second one is deliberate -- two prompt machines racing
    // for Confirm on the same screen is a bug waiting to be written.
    Resuming,
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
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More };
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: 0=Contents, 1=Text, 2=More
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font size / line spacing / alignment / orientation /
  // auto page turn. Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup overlayPopup;
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
  // True while this reader has put its PageFlip radio down for the build that is running, which is
  // the one case where the build must run to completion instead of stopping at BUILD_WINDOW_AHEAD:
  // the link cannot come back until the build is over, and an idle-but-live build never gets there.
  // A plain member read would not compile with paired reading switched off, hence the pair.
#ifdef FREEINK_CAP_PAGEFLIP
  bool pageflipBuildingForSuspendedLink() const { return pageflipHeapSuspended; }
#else
  static constexpr bool pageflipBuildingForSuspendedLink() { return false; }
#endif
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  std::string currentChapterTitle() const;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  void openDictionaryWordSelect();
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  // Applies a single page step to the current position. Requires a loaded `section`, and takes
  // the RenderLock itself when the step crosses a section boundary -- so it must not be called
  // from render(), which holds that (non-recursive) lock for its whole body.
  // Returns false when the step crossed a section boundary: the section is unloaded and the
  // landing page is only known once render() has loaded the neighbouring one, so a caller
  // stepping more than once must defer the rest rather than call again immediately.
  bool advanceOnePage(bool isForwardTurn, bool& moved);
#ifdef FREEINK_CAP_PAGEFLIP
  // Brings the pair link up for this book, or leaves the reader solo if it cannot start. Reading
  // never blocks on the peer. Does nothing at all when paired reading is switched off.
  void pageflipBegin();
  void pageflipEnd();
  // Which half of the spread this device is, from settings. Nothing can discover it -- only the
  // user knows which device they put on the left.
  static PageFlipRole pageflipConfiguredRole();
  // Reads SETTINGS.pageflipPeerMac into the session, or clears it (docs/pageflip.md section 8.1).
  // Returns true when a paired device is configured.
  bool pageflipApplyPeerMac();
  // Whether the status bar should carry the "configured but alone" badge right now.
  bool pageflipBadgeDue() const;
  // Brings the link up or down and adopts a role change, per pump. Neither setting has a
  // changed-hook to hang off: the web UI writes them straight into SETTINGS under a live reader.
  void pageflipReconcileSettings();
  // Pumped once per frame: drains owed steps, announces a settled local turn, applies one peer
  // packet.
  void pageflipPump();
  // Queues `steps` single-page steps and applies as many as can be applied right now. Both report
  // whether the position moved, which is what pageTurn() owes ReaderActivity.
  bool applyAdvance(bool forward, uint8_t steps);
  bool consumePendingAdvance();
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
  bool pageflipSettledPage(int32_t& spineIndex, int& page);
  // The same, plus the content offset the join negotiation compares (section 4.2). Separate because
  // it can fail where the page alone cannot: a section that is not loaded, or a page outside it,
  // has no anchor to report, and a greeting without one announces a position the peer cannot test
  // itself against. Turns keep using the page-only form -- they run between devices already proven
  // to share a layout, where a page number means the same thing on both.
  bool pageflipSettledPosition(int32_t& spineIndex, int& page, uint32_t& visibleTextOffset);
  // The position for a presence packet, which has to go out in places the join's anchor cannot be
  // read at all -- end of book most of all, where there is no section and never will be until the
  // reader leaves. Falls back to the last anchor read rather than staying silent.
  bool pageflipPresenceAnchor(int32_t& spineIndex, int& page, uint32_t& visibleTextOffset);
  // Remembers an anchor that was just read, so the heartbeat above it has something to say while a
  // render holds the lock.
  void pageflipRecordAnchor(int32_t spineIndex, int page, uint32_t visibleTextOffset);
  // Puts the link down for a chapter that has to be laid out from scratch, and brings it back when
  // that is done. Called from the main task at the points a section is dropped for another one.
  void pageflipSuspendForColdBuild();
  void pageflipResumeAfterColdBuild();
  // Whether the chapter the link was released for has finished, one way or the other. Taken under
  // the render lock, because the section it reads belongs to the render task.
  bool pageflipColdBuildOver();
  // Whether this suspension has run past PAGEFLIP_COLD_BUILD_DEADLINE_MS. Cheap and lock-free, so
  // the pump can ask it before paying for the lock the release below needs.
  bool pageflipColdBuildOverdue() const;
  // Persists the overdue build as a partial and frees its BuildContext, so the link can come back
  // without landing on top of it. Taken under the render lock: the section belongs to the render task.
  void pageflipReleaseOverdueBuild();
  // Backstop for builds started away from a chapter crossing (loop()'s partial extension,
  // render()'s blocking one): puts the link down when a live build can no longer be ticked, which
  // is the deadlock the crossing guard prevents by hand.
  void pageflipSuspendForStalledBuild();
  // Whether this spine has no finished layout on the SD card, so reaching it means a full build.
  bool pageflipSectionCacheMissing(int spineIndex) const;
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
  // `ownPage` and `ownOffset` are this device's settled position, passed in rather than re-read:
  // they were taken under the same lock as the verdict, and the divergent prompt has to describe
  // the position that was classified, not one the reader has moved to since.
  void pageflipApplyJoin(const PageFlipJoinResolution& resolution, int ownPage, uint32_t ownOffset);
  // Seeks to the position the user chose on the other device, then owes the role offset -- one step,
  // because a section boundary may sit between the chosen page and this device's.
  void pageflipResumeTo(const PageFlipDecision& decision);
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

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

  bool loadBook() override;
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  void loop() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;
#ifdef FREEINK_CAP_PAGEFLIP
  // The peer's traffic is this device's activity: it is being read from, just not pressed, and
  // today's inactivity timer would otherwise sleep it out from under the reader.
  bool preventAutoSleep() override {
    return pageflip && lastPeerContactMs != 0 && millis() - lastPeerContactMs < PEER_ACTIVITY_WINDOW_MS;
  }
#endif

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
