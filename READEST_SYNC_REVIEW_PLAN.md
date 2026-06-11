# Readest Sync — review fix plan (and cross-branch porting strategy)

> **Status 2026-06-11:** Phases 0–2 (items 1–12) are DONE on `readest-on-crumble`
> and ported to `readest-sync` (Phase 0 + 9 fixes cherry-picked; library-UX
> cancel adapted to upstream's HttpDownloader cancelFlag API + a
> consumeNextBackRelease member; goHome sliver hand-applied — upstream's
> menuItemToIndex already handled READEST_LIBRARY). Builds verified:
> crumble sim + tiny, readest-sync default. Sim repo gained
> useHTTP10()/getStream() stubs (commit a447daa).
> Remaining: Phases 3–5 (items 13–22).

Source: code review of `readest-on-crumble` (diff vs `crumble/main`, 63 files, +4,247).
Goal: fix the confirmed bugs and cleanups on `readest-on-crumble`, and land every
applicable fix on `readest-sync` (upstream-based, crumble-less) too.

## 1. Cross-branch reality check (verified 2026-06-11)

`readest-sync` and `readest-on-crumble` have **no shared git history** (no
merge-base — the CrumBLE port was a re-commit). But the Readest code itself is
almost byte-identical across both branches:

| Area | Divergence |
|---|---|
| `lib/ReadestSync/*` | Identical **except** `ReadestProgressMapper.{h,cpp}`: upstream's `toCrossPoint` takes a `GfxRenderer&` (XHTML-streaming xpath fallback); CrumBLE's doesn't |
| `lib/TestHooks`, `lib/Utils/NtpSync.*`, `lib/XmlParserUtils` | Identical |
| `src/activities/**/Readest*` | Identical except one line in `ReadestSyncActivity.cpp` (~137): the `toCrossPoint(..., renderer, ...)` call site |
| `src/JsonSettingsIO.{cpp,h}` | Diverges (each branch's file carries that branch's other settings) — the Readest serializers inside are the same logic |
| Integration hooks (`main.cpp`, `EpubReaderActivity`, `HomeActivity`, `ActivityManager`, `CrossPointWebServer`, `SettingsList.h`) | Branch-divergent — hunks must be ported by hand (they're all small) |

### Porting mechanics

- **Commit discipline is the whole strategy.** Every fix lands as either a
  `[lib]` commit (touches only branch-identical files above) or an
  `[integration]` commit (touches divergent files). Never mix.
- `[lib]` commits transfer with plain `git cherry-pick <sha>` — this works
  across unrelated histories as long as file content matches, which it does.
  Only commits touching `ReadestProgressMapper` / the one call-site line will
  conflict, and the resolution is always "keep this branch's renderer param".
- `[integration]` commits are re-applied by hand on `readest-sync` (or via
  `git diff <sha>^! -- <path> | git apply -3`); each is a few lines.
- Do **not** bulk-sync with `git checkout readest-on-crumble -- lib/ReadestSync`
  — it clobbers the intentional mapper divergence.
- Phase 0 below shrinks the divergent surface first, so everything after it
  ports more cheaply. Work on `readest-on-crumble` first (it's the active
  branch), cherry-pick to `readest-sync` per phase, not at the very end.

## 2. Phase 0 — keystone refactor: make `lib/ReadestSync` self-contained `[lib]`

**Move the Readest serializers out of `src/JsonSettingsIO` into a lib-local
`ReadestJsonIO`** (mirror `lib/KOReaderSync/KOReaderJsonIO`).

- Fixes the layering inversion: `lib/ReadestSync/*.cpp` currently include
  `"../../src/JsonSettingsIO.h"` (e.g. `ReadestBookCatalog.cpp:8`), coupling
  the lib to `src/` and to every settings store JsonSettingsIO drags in.
- **Porting payoff:** after this, `src/JsonSettingsIO` no longer contains any
  Readest code — the *only* remaining cross-branch divergence in Readest code
  is the mapper's renderer param. `readest-sync`'s JsonSettingsIO loses its
  Readest sections in the same commit (an `[integration]` sliver each side).

## 3. Phase 1 — severe bugs (fix first, each independently testable)

1. **`[lib]` Wild read on SD error** — `ReadestHash.cpp:79`:
   `file.read()` returns signed `int` (−1 on I/O error) stored into `size_t`,
   evading `bytesRead == 0` → `md5.add(buf, SIZE_MAX)`. Fix:
   `int n = file.read(...); if (n <= 0) return "";`.
   While there: same latent pattern in `KOReaderDocumentId.cpp:79` (both branches).
2. **`[lib]` meta_hash matches discarded** — `ReadestSyncClient.cpp` `pullConfig`:
   only accepts `parsed.bookHash == bookHash`, dropping rows the server matched
   via the spec §5.3 meta_hash OR-filter (the cross-device, byte-different-file
   case). Accept `parsed.metaHash == metaHash` too; decide push-hash policy
   (push under local book_hash but adopt remote row's hash? follow spec §5.3).
3. **`[lib-act]`¹ Missing silent restart on exit** — `ReadestSyncActivity.cpp`
   `onExit` (~228) and `ReadestLibraryActivity.cpp` `onExit` (~51): call
   `silentRestartToReader()` / `silentRestart()` like KOReaderSyncActivity and
   OpdsBookBrowserActivity do (main.cpp:439 documents why — post-TLS heap
   fragmentation). Verify the helper exists on `readest-sync` (it should: the
   activity was cloned from kosync which has it upstream).
4. **`[lib]` Unbounded first library pull** — `ReadestStorageClient.cpp`
   `pullBooksSince`: replace `getString()` + unfiltered `deserializeJson` with
   `deserializeJson(doc, http.getStream())` + a `JsonDocument` filter for the
   ~8 fields `rowToBook` reads. Also consider a paging/limit param if the
   server supports it; today a failed pull retries `since=0` forever because
   the cursor only advances on full success.

¹ `[lib-act]`: file is in `src/activities` but byte-identical across branches → cherry-picks like `[lib]`.

## 4. Phase 2 — remaining correctness bugs

5. **`[lib-act]` `pulled.deleted` ignored** — `ReadestSyncActivity.cpp:112`:
   treat a soft-deleted config row as "no remote progress" (spec: do not treat
   deleted rows as state), instead of offering Apply Remote → start-of-book.
6. **`[lib-act]` Empty-hash guard** — put the guard in
   `ReadestSyncActivity::performSync` (mirrors kosync, which guards in
   performSync, and keeps the fix portable) rather than in the divergent
   `EpubReaderActivity` menu case: empty `bookHash`/`metaHash` → `SYNC_FAILED`
   + `STR_HASH_FAILED`. Prevents junk empty-book_hash rows on the server.
7. **`[lib-act]` Download format handling** — `ReadestLibraryActivity.cpp:~277`:
   stop hardcoding `.epub`; derive extension from `BookRow.format` or filter
   `fetchBooks` to supported formats. Also add a uniquifier (short hash prefix)
   to the filename so same-title books don't silently overwrite each other,
   and don't `recordDownload` a file the reader can't open.
8. **`[lib-act]` Cancellable downloads** — `ReadestLibraryActivity.cpp:~281`:
   pass `cancelFlag`/`shouldCancel` to `HttpDownloader::downloadToFile` and
   handle `ABORTED`, exactly as OpdsBookBrowserActivity does.
9. **`[lib-act]` Loading screen never paints** — `ReadestLibraryActivity` onEnter
   / ERROR-retry: adopt OPDS's `showLoadingBeforeFetch()` →
   `requestUpdateAndWait()` before the blocking TLS fetch.
10. **`[integration]` Password can never be cleared** — web POST skips empty
    write-only values (`CrossPointWebServer.cpp:1482`) and the keyboard path
    skips empty input (`ReadestSettingsActivity.cpp:124`); `clearSession()`
    preserves the password. Add an explicit clear path (sign-out option that
    also wipes the password, or a dedicated Clear action in the settings
    activity). Settings-activity half is `[lib-act]`; web-server half is per-branch.
11. **`[integration]` goHome mapping** — add `"ReadestLibrary"` to
    `ActivityManager::goHome()`'s name switch + a `HomeMenuItem::READEST_LIBRARY`
    mapping in `homeActionForInitialMenuItem`, so backing out re-selects the
    entry (parity with OPDS). Tiny, but both files diverge per branch.
12. **`[lib]` recordSyncResult drift** — `ReadestStorageCoordinator` never calls
    `READEST_STORE.recordSyncResult()` (SyncCoordinator does), so library/
    download failures never reach the settings "Last Error". Fixed for free by
    Phase 3's `withAuthRetry` unification — or one line now.

## 5. Phase 3 — dedup / structural improvements

13. **`[lib]` Shared HTTP layer** — one `ReadestHttp::requestJson(...)` replacing
    the triplicated `extractErrorMessage`/`mapHttpStatus`/`configureTls`/
    `addAuthHeaders` + the ~25-line request block in all three clients. Put
    kosync's `MIN_HEAP_FOR_TLS` pre-flight check here (Readest currently has
    none). Phase 1.4's streaming parse should land as part of this helper.
14. **`[lib]` `withAuthRetry` template** — collapse the five hand-rolled
    call→AUTH_EXPIRED→refresh→retry wrappers across the two coordinators.
15. **`[lib]` One partial-MD5 implementation** — `ReadestHash::partialMd5` and
    `KOReaderDocumentId::calculate` produce byte-identical offset tables
    ({0, 1024, 4096, …, 1<<30}, 1 KB chunks, lowercase hex); the
    `ReadestHash.h:41` "different offset table" comment is wrong. Share one
    implementation (param: skip-vs-abort on read failure); fix the comment.
16. **`[lib-act]` Sync-activity dedup** — `wifiOff`/`ensureEpubLoaded`/
    `saveProgressAndReturn`/`returnToReader` are verbatim KOReaderSyncActivity
    clones; extract a common base or helpers. Medium-size; do after the bug
    fixes so cherry-picks stay clean. Migrate kosync to `lib/Utils/NtpSync`
    (delete `KOReaderSyncActivity::syncTimeWithNTP`) in the same pass.
17. **`[lib]` Dead code removal** — `needsLogin`/`needsRefresh`/`expiresIn`/
    `lastConfigsSyncAtMs` (+ misleading `clearSession` comment — the real pull
    cursor is `ReadestBookCatalog::cursorMs`); `hasRemote`/
    `currentParagraphIndex`/`CONNECTING` in ReadestSyncActivity;
    `tagEqualsWithPrefix`.

## 6. Phase 4 — heap / efficiency

18. **`[integration]` Lazy-load catalog** — `main.cpp:1095-1097`: keep only
    `READEST_STORE.loadFromFile()` at boot (home-menu gate needs it);
    move `READEST_LIB_STORE`/`READEST_CATALOG` loads into
    `ReadestLibraryActivity::onEnter` so the catalog vector isn't resident
    for the device's whole uptime. Loader calls themselves are `[lib]`-side.
19. **`[lib-act]` Misc** — `fetchBooks` should hold indices into
    `READEST_CATALOG.getBooks()` instead of deep-copying rows; `mergeDelta`
    builds a hash→index map (or replaces wholesale when `cursorMs == 0`);
    `downloadBook` reuses one `WiFiClientSecure` across list→sign→download
    (3 TLS handshakes today, ~2-4 s each); cache the two book hashes (inside
    `ReadestHash`, keyed by path+size+mtime) instead of re-hashing + re-parsing
    the OPF on every sync invocation.
    > DONE except two sub-items deliberately skipped: TLS connection reuse
    > conflicts with the HTTP/1.0 streamed parse (no keep-alive under 1.0 —
    > OOM safety won); hash caching buys nothing per-boot because the sync
    > flow silent-restarts on exit (a persistent cache would need mtime
    > plumbing — revisit only if sync-entry latency actually bothers anyone).
20. **`[integration]` HomeActivity snapshot** — query
    `READEST_STORE.hasCredentials()` once per refresh into a member (like
    `hasOpdsServers`) instead of live in `buildHomeMenuItems`/
    `buildMinimalMenuItems`/`getMenuItemCount` (lines 248/271/537).

## 7. Phase 5 — build & packaging (mostly CrumBLE-only)

21. **TLS config out of test_hooks** — production CA-bundle wiring lives in
    `src/test_hooks/` + `lib/TestHooks/`; committed `[simulator_base]`
    `build_src_filter` does **not** exclude `src/test_hooks/` and
    `scripts/sim_c_std.py` is only wired via the gitignored
    `platformio.local.ini` → a fresh clone's sim build likely fails to link
    (`_binary_x509_crt_bundle_start`). Verify with a fresh-clone sim build,
    then commit the seam properly (sim stub next to the other sim_stubs, ini
    filters in the committed file). Check how this manifests on `readest-sync`
    too — its platformio.ini differs.
    Bonus: switch kosync's `setInsecure()` (3 sites) to `ReadestTls::configure`.
22. **CrumBLE-only:** `embed_wasm.py` docstring still advertises the old
    uppercase header names; case-rename can leave a stale uppercase header in
    trees built pre-rename (Windows/WSL dual view). N/A on `readest-sync`.

## 8. Verification per phase

- `test/run_readest_hash_test.sh` (+ add a regression case for read-error → ""
  to cover Phase 1.1; ref impl `scripts/readest_hash_ref.py`).
- Simulator script driver (`--script`, `scripts/run_sim_script.sh`) for the UI
  flows: library browse/download/cancel, sync compare screen, settings clear.
- Self-hosted Readest stack (Docker, see memory notes) for end-to-end:
  meta_hash cross-file pull (1.2), deleted-row handling (2.5), large-library
  pull (1.4).
- Both branches: `pio run -e <device-env>` + sim build after each phase's
  cherry-pick round.

## 9. Suggested execution order

| Round | Items | Then port to `readest-sync` |
|---|---|---|
| 1 | Phase 0 (keystone) | cherry-pick + JsonSettingsIO sliver |
| 2 | Phase 1 bugs 1–4 | cherry-pick (mapper-line conflict in 1.3 only) |
| 3 | Phase 2 bugs 5–12 | cherry-pick `[lib]`/`[lib-act]`; hand-port 10(web), 11 |
| 4 | Phase 3 dedup 13–17 | cherry-pick |
| 5 | Phase 4 + 5 | cherry-pick lib side; hand-port main.cpp/Home hunks; 21 per-branch |
