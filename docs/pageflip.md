# PageFlip — two-device two-page spread over ESP-NOW

**Status:** design confirmed; implementation started at §9. Landed so far: the `advanceOnePage()`
extraction (step 2's prerequisite — see the correction in §3)
**Branch:** `pageflip` (off `origin/master` @ `95a847c7`, in sync with upstream v1.5.0)

Two X4s side by side act as one two-page spread: left device shows page *P*, right device
shows *P+1*, and a page turn advances the pair by two.

---

## 1. The core insight

A reader position in this codebase is two ints — `currentSpineIndex` and `nextPageNumber`
([EpubReaderActivity.h:17-18](../src/activities/reader/EpubReaderActivity.h)). If both devices
hold the same EPUB and render locally, the sync payload is **tens of bytes per page turn**, not
a 48KB framebuffer.

E-ink refresh is 300ms–1s, so any transport under ~100ms is invisible. **Bandwidth and latency
are non-problems.** The real problems are link lifecycle, power, and render determinism.

---

## 2. Why ESP-NOW and not BLE

`upstream/feat-bluetooth` (BLE HID page-turner) is the control experiment. It is **+2224 lines
across 53 files**, and a large fraction is heap surgery done purely to fit NimBLE alongside the
reader:

| Commit | What it was fighting |
|---|---|
| `28af4189` Chunk font bitmap allocations to avoid fragmentation | heap fragmentation |
| `f45f75f9` Add emergency buffer for font glyph rendering | OOM under BLE |
| `7dbfcbb5` Split font scan buffers by text style | peak RAM |
| `200abc6a` Replace vector with deque for ParsedText tokens | fragmentation |
| `2c8bd8ea` Fix heap check to exclude framebuffer from threshold | headroom accounting |
| `scripts/patch_bt_mem.py` (+95) | patching the framework to reclaim BT memory |
| `BtLibraryInUseShim.cpp`, `_btLibraryInUse` weak symbol | BLE controller link friction |

That is the cost of standing up a **second** radio stack on a 380KB chip.

ESP-NOW avoids all of it:

- **No new stack.** `WiFi.mode()` is already called throughout `src/` (web server, OTA, KOReader
  sync, font download). The WiFi stack is a cost the firmware already pays.
- **Connectionless.** Nothing to re-establish after a cold boot — store the peer MAC, send.
  This directly answers the lifecycle problem in §4.
- **Symmetric.** No central/peripheral roles; either device can originate a message.
- **The SDK already ships an ESP-NOW transport.** See below — this is the strongest argument of
  the four and an earlier draft of this document got it backwards.

### `freeink-sdk` already has the transport

An earlier draft claimed "clean slate — no `esp_now` references anywhere". **That was wrong**, and
wrong for an instructive reason: the check was run while the `freeink-sdk` submodule was still
uninitialised, so it searched an empty directory.

The SDK ships [`libs/network/NearbyTransfer`](../freeink-sdk/libs/network/NearbyTransfer) —
*"Low-memory ESP-NOW transport and reliable file-transfer session primitives for FreeInk devices"*:

```cpp
namespace freeink::nearby {
  class EspNowTransport {
    bool begin(uint8_t channel);
    bool send(const uint8_t* destinationMac, const uint8_t* data, size_t length);
    bool poll(Event& event);          // 4-deep event queue, ISR-safe enqueue
    bool localMac(uint8_t* output) const;
  };
  enum class PacketType { Discover, Advertise, Offer, Accept, Reject,
                          Data, Ack, Complete, Result, Cancel };
  bool encodePacket(...); bool decodePacket(...);   // type / sessionId / sequence framing
  class ReliableTransferSession { ... };            // chunked, sequenced, CRC32
}
```

**PageFlip does not implement an ESP-NOW transport — it wraps this one.** The poll-based `Event`
queue maps directly onto the per-frame pump in §8, and `Discover`/`Advertise`/`Offer`/`Accept`
already give pairing a wire vocabulary.

Two caveats found by reading the implementation, both of which matter later:

- **It was not in the build.** `platformio.ini` symlinked twelve SDK libs and `NearbyTransfer` was
  not among them. Now added — a one-line `lib_deps` entry, as expected, not a port. It costs ~11 KB
  of flash and only compiles when something includes the wrapper.
- **`begin()` returns `false` under `SIMULATOR`** — the whole body is inside
  `#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)`, with `#else return false`
  ([NearbyTransfer.cpp:155-157](../freeink-sdk/libs/network/NearbyTransfer/src/NearbyTransfer.cpp)).
  This makes the UDP-loopback shim in §11 mandatory rather than merely convenient.
- **It costs ~4.4 KB of RAM to hold, sized for a job PageFlip does not do.** `EspNowTransport`
  embeds `std::array<Event, 4>`, and each `Event` carries a `std::array<uint8_t, MAX_PACKET_BYTES>`
  with `MAX_PACKET_BYTES = 1100` — a queue dimensioned for `ReliableTransferSession`'s 1 KB file
  chunks, while a PageFlip packet is 25 bytes. On a 380 KB device that is worth knowing before it
  shows up as a heap regression. It is not a blocker (the object is a single long-lived instance,
  and 4.4 KB is affordable), and the honest options if it ever is: hold it only while pairing is
  active, or call `esp_now_register_recv_cb` directly from the ESP-NOW implementation of the §11
  interface and keep a 4×64 B queue. Do not fork the SDK library to shrink the constant.
  **Taken: the first one.** `PageFlipEspNowTransport` holds the SDK transport and its receive
  scratch (~5 KB together) behind a pointer that exists only between `begin()` and `end()`, so a
  device reading solo pays nothing. Both are far too large to be stack locals in any case.

### Verified APIs (checked in the prebuilt C3 framework, not from memory)

```
esp_now_set_wake_window(uint16_t window)
  framework-arduinoespressif32-libs/esp32c3/include/esp_wifi/include/esp_now.h:382

esp_wifi_connectionless_module_set_wake_interval(uint16_t wake_interval)
  .../esp_wifi/include/esp_wifi.h:1505

CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y
  framework-arduinoespressif32-libs/esp32c3/sdkconfig
```

Both headers note the connectionless power-save path works *while disconnected* only when
`ESP_WIFI_STA_DISCONNECTED_PM_ENABLE` is set — and the prebuilt libs already set it. So
duty-cycled ESP-NOW RX with no AP association is supported on this exact toolchain.

---

## 3. Protocol: absolute position, local advance

### Why not inject a virtual button

`feat-bluetooth` overlays synthetic button edges onto `MappedInputManager`
(`blePressEdge[kButtonCount]`, `MappedInputManager.h`). That is right for a clicker and **wrong
here**: a dropped packet leaves the peer permanently one page out of step, and nothing ever
corrects it.

### No authority owner: each device advances itself by two

There is **no global page index** for EPUB — book progress is derived from byte position
(`bookSize`, [EpubReaderActivity.cpp:698-700](../src/activities/reader/EpubReaderActivity.cpp));
only XTC has a real `bookPageCount`. So "left shows even pages" is not expressible. The pair is a
floating `(P, P+1)` window.

Given that, the model is: **each device owns its own position and advances it by two.**

```
LEFT   P    -> P+2
RIGHT  P+1  -> P+3
```

The invariant `right = left + 1` is preserved without either device computing from the other's
position. Consequences:

- **No authority owner.** Nothing to arbitrate, no left/right master.
- **A press on the right device does not invert the pages** — the failure mode of a naive
  "whoever pressed becomes leader" scheme.
- **No extra hop** for right-device presses.

Role (left/right) remains a fixed per-device setting; it determines the join offset in §4, not
who is in charge.

### The packet

Broadcast by whichever device turned the page. As implemented
([lib/PageFlip/PageFlipPacket.h](../lib/PageFlip/PageFlipPacket.h)) it is **25 bytes**:

```
magic        u16   'PF'  (0x50 0x46 on the wire — little-endian throughout)
protoVer     u8
message      u8    Turn | Hello | SyncOffer | SyncAnswer | SyncApply | JoinResume
flags        u8    role(left/right), dir(fwd/back), atBookEnd
compatHash   u32   see §5
bookId       u32   existing EPUB path hash
turnSeq      u32   shared logical turn count — dedup key AND drift detector
spineIndex   i32   sender's own resulting position
pageNumber   i32
```

A **hello** is these 25 bytes plus four more: the join's `visibleTextOffset` (§4.2). The offset is
*appended* past the turn's last field rather than inserted, so every shared field stays where a
turn's is and one set of constants still writes both. Its flags byte carries three more bits the
turn does not use — the join verdict (two bits) and "this greeting starts a new join". The
force-sync messages of §5.1 and the resume of §4.3 share the header and the hash/bookId pair, then
diverge.

Three things settled while building it:

- **`message` is new** — an earlier draft of this list had no type discriminator, but §4's `HELLO`
  rides the same transport, so one is needed. Adding it now costs a byte; adding it later breaks the
  wire. `peekMessage()` reads it from the 4-byte header alone, so a receive loop dispatches before
  committing to a decode. It has since earned its place four times over: the three force-sync
  messages and the resume choice all ride the same transport.
- **The packet *is* the ESP-NOW payload.** The SDK's `encodePacket()` framing is not used: its
  16-byte header would nearly double a 25-byte packet to carry a type, session id and sequence that
  `message` / `bookId` / `turnSeq` already cover. PageFlip wraps the SDK's *transport*
  (send/poll/localMac), not its packet format.
- **Encoding is explicit little-endian, byte at a time, never a cast over the buffer** — the RISC-V
  unaligned-load rule applies to the wire exactly as it does to the cache deserialization code.
  `WireLayoutIsPinned` in the unit test pins the 25 bytes, so a field reorder fails in CI rather
  than as a desync between two X4s.

Decoding rejects foreign traffic (wrong magic, unknown `protoVer`, unknown `message`) and any short
buffer, but **tolerates trailing bytes** so a later version can append fields without breaking
today's receivers. That matters on a broadcast medium shared with whatever else is on the channel.

`pageNumber` is safe **here** — and only here — because `compatHash` rides in the same packet, so
a turn is only ever applied between devices already proven to share a layout. The *join* path
cannot assume that and uses `visibleTextOffset` instead (§4.2). Do not unify the two: steady-state
turns want the cheap comparable integer, joins need the layout-independent anchor.

### Apply rule

Implemented in [`PageFlipSession`](../lib/PageFlip/PageFlipSession.h), which decides and returns a
`PageFlipAction` (`Ignore` / `AdvanceTwo` / `Heal` / `Mismatch`); the reader applies it. Keeping the
decision out of the activity is what lets the awkward cases — dedup, drift healing, simultaneous
presses, the lower-MAC tiebreak — be driven by host tests against two real sessions on real
sockets, instead of only through the UI.

On a local press: `turnSeq++`, advance own position by two, broadcast.

On receive, compare `turnSeq` against the local one:

| Incoming | Meaning | Action |
|---|---|---|
| `<= local` | already applied, or a simultaneous duplicate | **ignore the advance**, but still use the absolute position as a consistency check |
| `== local + 1` | the expected next turn | advance own position by two (fast path) |
| `> local + 1` | turns were missed | do **not** replay — seek absolutely from the sender's position, offset by the role delta (heal) |

### `turnSeq` must be adopted at join, not started from zero

`turnSeq` is a **session** counter, and a cold boot resets it. That is a deadlock if left
unhandled: suppose one device sleeps and the other does not.

```
rebooted device:  turnSeq = 0
awake device:     turnSeq = 847

rebooted presses -> broadcasts turnSeq = 1
awake device     -> 1 <= 847 -> "already applied" -> IGNORED
```

Presses on the rebooted device would do nothing at all, permanently, while the reverse direction
takes the heal path and appears to work — a maddening half-broken state.

**The `HELLO` exchange adopts `max(local, peer)` for `turnSeq`, exactly as it adopts position.**
It is part of the join, not merely a runtime counter. Persisting it to NVS alongside the peer MAC
is a cheap belt-and-braces, but adoption at join is what actually makes it correct — a device
whose peer was replaced or reflashed still converges.

This keeps the hybrid property that matters:

- **Absolute position → self-healing.** Any dropped or reordered packet is corrected by the next
  one; no permanent drift.
- **Local advance → boundary handling for free.** This is the part that matters most (below).

### Simultaneous presses

Both devices pressing within the same window independently compute the same `turnSeq = N+1`, so
each sees the other's packet as "already applied" and ignores it. The pair advances once, not
twice — the double-advance to `P+4/P+5` is exactly what the counter prevents.

**Opposite directions** in the same window (one forward, one back) is the residual race: both emit
`N+1` with different `dir`, each ignores the other, and the pair desyncs. Tiebreak on lower peer
MAC; the loser heals from the winner's absolute position. Rare, but it must not be left undefined.

### The boundary problem this solves

`pageTurn()` ([EpubReaderActivity.cpp:1040-1074](../src/activities/reader/EpubReaderActivity.cpp))
shows why a device cannot just compute its peer's page arithmetically:

```cpp
if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
  section->currentPage++;
} else {
  RenderLock lock(*this);
  nextPageNumber = 0;
  currentSpineIndex++;
  section.reset();
}
```

Sections build **lazily and windowed** — `pageCount` is a moving watermark, not a total. The
device itself does not reliably know it is on the last page of a section. And paging backward
across a boundary uses a sentinel, `pendingPageJump = UINT16_MAX`, meaning "last page of the
previous section, whatever that turns out to be" (line 1066).

So each device must run *the same algorithm* against *its own* section object rather than trust
arithmetic from the peer. Extract the body of `pageTurn()` into a reusable single-step helper and
both paths share one implementation.

### Correction: the helper is a mutating member, and the second step must be deferred

An earlier draft specified a free function `advanceFrom(spine, page, forward)` returning a new
position, with *"both the local-press path and the receive path call `advanceFrom()` twice"*.
**Both halves of that are wrong**, and reading the function is what showed it:

- **It cannot be a pure function of `(spine, page)`.** The forward branch tests
  `section->currentPage < section->pageCount - 1 || section->isBuilding()` — the decision needs the
  *loaded `Section` object*, not two ints, which is the same reason §3 gives for not computing the
  peer's page arithmetically. It mutates `nextPageNumber` / `currentSpineIndex` / `pendingPageJump`
  and takes a `RenderLock`, so it stays a member of `EpubReaderActivity`.
- **It cannot be called twice in a row.** The boundary path ends in `section.reset()`, and every
  caller of `pageTurn()` guards on `if (!section)` first ([EpubReaderActivity.cpp:462, :670]).
  A second immediate call would dereference a null `section`. Worse, the backward crossing sets
  `pendingPageJump = UINT16_MAX` — *"last page of the previous section, whatever that turns out to
  be"* — so the landing page of the first step is genuinely unknown until `render()` has loaded and
  built the neighbouring section.

**Implemented shape** (`EpubReaderActivity::advanceOnePage`, extracted from `pageTurn()`):

```cpp
// Returns false when the step crossed a section boundary: the section is unloaded
// and the landing page is only known once render() has loaded the neighbouring one.
bool advanceOnePage(bool isForwardTurn);

void pageTurn(bool isForwardTurn) {   // unchanged behaviour: one step + turn bookkeeping
  advanceOnePage(isForwardTurn);
  lastPageTurnTime = millis();
  requestUpdate();
}
```

So a two-page advance is **step, and if the step crossed a boundary, defer the remainder** — a
small `pendingAdvanceSteps` counter, consumed once the neighbouring section is loaded.

**Consume it in `loop()`, never in `render()`.** `advanceOnePage()`'s boundary branches construct a
`RenderLock`, and that is a plain non-recursive FreeRTOS mutex —
`xSemaphoreTake(renderingMutex, portMAX_DELAY)` with no holder check
([ActivityManager.cpp:326-341](../src/activities/ActivityManager.cpp)). `render(RenderLock&&)`
holds it for its entire body (it never unlocks; the caller's lock outlives the call), so a deferred
step taken inside `render()` would take the same mutex from the same task and block forever. Note
`applyDeferredReposition()` *is* called from inside `render()` — it is safe there precisely because
it only moves `section->currentPage` and never constructs a lock. Don't read it as a precedent.

The consume point is therefore the same place `loop()` already runs its other deferred section work
(background build tick, deferred reposition): guarded by `section && !RenderLock::peek()`, apply one
step, `requestUpdate()`. Each loop iteration consumes at most one step, so a single-page section
converges over successive iterations rather than looping in place.

Solo mode requests one step, pair mode two. That remains the only behavioural difference between
paired and solo reading — but the mechanism is a deferred counter, not two calls.

### End of book

Mid-book section boundaries need no special handling — `P+1` rolls into the next section via the
same helper. Only the true end of the book is special, because there is no `P+1`.

**The right device shows a static "End of book" panel; interaction stays on the left.**

```
LEFT: [ last page ]          RIGHT: [ End of book ]
        press forward
LEFT: [ EndOfBookOptions ]   RIGHT: [ End of book ]
        Open next / Home / Last page
```

`EndOfBookOptions` already exists with `Action::{None, Redraw, OpenBook, GoHome, LastPage}`
([EndOfBookOptions.h:16](../src/activities/reader/EndOfBookOptions.h)) and is reused as-is on the
left. Only one device accepts input, so there is never a question of which screen you are
answering. Two devices showing the same last page was rejected — it reads as a sync bug rather
than an intentional end state.

---

## 4. Lifecycle — the top risk, and it is UX not radio

On X4, deep sleep pulls GPIO13 low to drop the battery latch MOSFET
([HalPowerManager.cpp:66-79](../lib/hal/HalPowerManager.cpp)):

```cpp
// X4 GPIO13 is connected to the battery latch MOSFET. Keeping it low powers
// the MCU off on battery, while the SDK wake source still handles USB power.
```

**The MCU powers off completely.** There is no persistent link to maintain, because there is no
persistent anything. Every wake is a cold boot, and the two devices have independent power
buttons and uncorrelated wake times.

Required behaviour:

- **On entering the reader with pairing configured:** broadcast `HELLO` carrying own position,
  `compatHash` and `turnSeq`, then run the join negotiation below. No reply within ~1.5s → proceed
  solo, show a "peer offline" indicator. Never block reading on the peer.
- **Peer traffic must count as user activity.** A device being read from but not pressed
  receives no button presses, so
  today's inactivity timer would sleep it out from under the reader. `lastActivityTime`
  ([main.cpp:494-499](../src/main.cpp)) must be reset on received packets — mirroring how
  `feat-bluetooth` added `bleHadActivityThisFrame()` for exactly this reason.
  **Only a fully decoded packet counts.** ESP-NOW is a broadcast medium, and the receive path
  deliberately dispatches on the 4-byte header before decoding, so "looks like ours" and "is ours"
  are different answers. Resetting the sleep timer on the former lets any stray broadcaster on the
  channel keep the device awake indefinitely — a battery bug that would only ever reproduce in
  someone else's room.
- **Role is fixed, authority is not a concept.** Left/right is a per-device display setting that
  sets the join offset (§3). Either device may originate a turn; neither is in charge.

### Presence has to expire, and nothing in the protocol was expiring it

Presence is what licenses advancing by two. A peer that powers off or walks out of range says
nothing on its way out, so without an expiry a device goes on turning **two pages per press with
nothing on the other end showing the second one** — solo reading that silently skips every other
page. That is the worst failure mode the feature has, because it reads as the *book* being broken
rather than the pair.

It cannot be expired on silence alone, because a settled pair has nothing to say: until step 5 the
only evidence of a peer was a page turn, so two devices sitting idle produced no traffic at all.
Hence a **heartbeat** — the same "I am here, this is my layout, this is where I am" packet the
mismatch path already answers greetings with. It asks for nothing back and starts no join round,
which is exactly what both jobs need: two devices doing it do not talk each other into an
ever-growing exchange, and a heartbeat that restarted the join would re-classify the pair every
couple of seconds.

Presence drops after four missed heartbeats, and the pair falls back to one page per press with a
notice. Two constraints on the rate:

- It has to sit comfortably inside the auto-sleep activity window above, or a device being read
  from but not pressed still sleeps out from under its reader — the heartbeat is what finally makes
  that bullet work, since before it the window could only ever be satisfied just after a turn.
- §7's duty cycling will want to revisit it. It is a power knob, and it is the one number in this
  section chosen for correctness rather than measured.

Resolving the content anchor for each heartbeat would be a file read once the chapter finalizes, so
it is cached per position: a heartbeat for a page the reader has not left costs nothing, and the
cache is dropped whenever the pagination moves.

Two conditions on when it is sent at all, both of which were wrong first:

- **Only once a peer has actually been seen this session.** A reader with no pair must go on paying
  nothing for the link — `pageflipEnd()`'s own comment says so — and pairing is not yet even a
  setting a user can turn off (§9 step 8). Discovery does not need the heartbeat: a device booting
  second sends a *greeting* on its first render, and the already-running device answers it. The
  condition is "has ever been in contact", deliberately not "is present right now": if a transient
  outage expired presence on both halves at once, gating on presence would stop both heartbeats and
  neither could re-acquire the other — the `turnSeq` deadlock shape again.
- **It must survive having no section.** At end of book there is none and never will be until the
  reader leaves: `render()` takes the end-panel early return before anything is loaded, and the
  forward crossing that got there already reset it. A heartbeat that required a readable anchor
  would go silent forever and the peer would call the device offline — dismantling the exact
  arrangement §3 designs for, where the right half shows the end panel and interaction stays on the
  left. The anchor only ever feeds a join probe, and a device with no section has nothing to join
  on, so presence falls back to the last anchor read.

### 4.1 Each device persists the page it was actually showing

Each device writes its own `progress.bin` with **the page it was displaying at disconnect** — the
right device stores `P+1`, not `P`. No rewriting to a notional pair base.

The consequence is that two freshly-woken devices will essentially *never* hold identical
positions, and that is expected, not an error. The join negotiation classifies the difference.

### 4.2 Join negotiation — anchored on text offset, not page number

**Page numbers are not portable and must never be compared across devices whose layouts might
differ.** Page 3 of chapter 7 under one font is not page 3 under another, and a force-sync (§5.1)
invalidates the cache so that the receiving device's *own* saved page number refers to a layout
that no longer exists.

The codebase already solved this. `CrossPointPosition` carries `visibleTextOffset`,
`paragraphIndex` and `xpathAnchorId` ([ProgressMapper.h:14-25](../lib/KOReaderSync/ProgressMapper.h))
precisely because page indices are unstable across render settings —
`EpubReaderUtils::saveProgress()` already persists the offset
([EpubReaderActivity.cpp:1554](../src/activities/reader/EpubReaderActivity.cpp)), and
`getPageForVisibleTextOffset()` (line 758) maps it back to a page under whatever layout is current.

**Join exchanges `(spineIndex, visibleTextOffset)`, never `(spineIndex, pageNumber)`.**

### Ordering is mandatory

```
1. compare compatHash
2. mismatch -> force-sync (§5.1), rebuild layout
3. re-anchor BOTH saved positions via visibleTextOffset -> getPageForVisibleTextOffset()
4. only now classify aligned / swapped / divergent
```

Running the classification before step 3 compares page numbers from two different layouts, which
is meaningless. This ordering is why step 4 precedes step 5 in §9.

### Classification: each device tests forward from itself

Neither device loads a section it does not already have. Each answers one cheap local question —
*"does my next page start at the offset my peer reported?"* — and the pair combines the two
booleans:

| Left says | Right says | Case | Action |
|---|---|---|---|
| next == peer | — | **Aligned** | resume silently — the normal reopen |
| — | next == peer | **Swapped** | re-normalise to role, resume silently. The user physically swapped the devices |
| no | no | **Divergent** | **prompt on both devices** |

`advanceFrom()` is only ever called on a device's *own* loaded section — the asymmetric case
(testing the reverse direction from the other device's position) would require loading a section,
possibly a whole chapter, that the testing device does not have.

### The table is missing the most common join, and it is the one this step exists for

Two devices on **the same page**. That is what a book opened for the first time on both looks like,
and what two devices resumed from the same synced progress look like. Under the table above it
falls to no/no — **Divergent** — so the pair would be prompted to choose between two positions that
are the same position, on essentially every first pairing. It is also the only join that has to
*create* the spread rather than recognise one: without it the pair sits at `(P, P)` forever, which
is exactly the lockstep behaviour step 2 deliberately left in place for step 5 to remove.

So there is a fourth case, **Identical** → *the right device steps forward one page; the left stays
where it is.* Silent, and it is where the one-page offset comes from in the common case.

**It cannot be read off the two booleans.** From the same offset each device finds its own next
page somewhere the other is not, so *both* answer "no" — indistinguishable from Divergent. It is a
direct equality test on `(spineIndex, visibleTextOffset)`, which either device can run alone the
moment it has the peer's position. That makes the common join one round trip instead of three, and
it is only sound because the negotiation runs behind the layout agreement of §5: the same offset
under two different paginations is not the same page.

Two consequences worth writing down, because both were mistakes on the way in:

- **The fix-up step must not be announced as a turn.** Sending it would bump `turnSeq` and the peer
  would advance two pages off it — breaking the spread on the join that created it. It is a local
  reposition: `applyAdvance()` without the announce flag.
- **A device on the last page of its section can never answer "next == peer" the ordinary way.**
  There is no next page to look up, and resolving it needs the following section, which this
  section forbids loading. Left on the last page of chapter *N* with right on page 0 of *N+1* — an
  ordinary aligned pair straddling a boundary — would answer Unknown forever and end up classified
  Divergent. It does not have to load anything: **page 0 of any section starts at offset 0**, so
  `peer.spineIndex == mine + 1 && peer.offset == 0` answers it. Gate that branch on the section
  being *finalized* — `pageCount` is a moving watermark while a build runs (§3), so "am I on my last
  page" has no answer until it settles.

`Unknown` is a third verdict, not a missing one, and it means **retry — never "no"**. A section
still building that answered "no" would classify a healthy pair as divergent and put a resume
prompt in front of the user for nothing. A device that answers Unknown also asks for no reply: two
of those would otherwise trade greetings at packet rate for as long as their builds ran, and the
retry it actually needs is its own re-greeting once its section settles.

### The classification happens in one place, reached by both devices

Both verdicts become known on one device when the peer's arrives and on the other when it computes
its own. If only the receive path could conclude, the join would work in one direction and be
silently dead in the other — the same half-broken pair the `turnSeq` adoption rule exists to
prevent. Everything resolves in `answerJoin()`, which every device reaches.

The greeting states on the wire whether it *starts* a join rather than that being inferred from
"asks for a reply". A reply that asks for one more round is not a fresh join, and restarting on one
classifies the pair twice — under Identical that steps the right device forward two pages instead
of one. The unit test for two simultaneous greetings is what caught it.

### 4.3 Divergent: prompt on both, confirm on one

When the two saved positions are genuinely unrelated (the devices were read separately), **both
devices show a resume prompt describing their own stored position. Confirming on either device
selects that device's progress for the pair**; the other dismisses its prompt and seeks to the
chosen position plus its role offset.

```
LEFT                          RIGHT
Resume from here?             Resume from here?
  Ch. 7 · p.3 · 34%             Ch. 12 · p.1 · 58%
  [Confirm]                     [Confirm]

  -- confirm on LEFT --

  Ch. 7 · p.3                   Ch. 7 · p.4
```

The interaction *is* the choice — no "which one?" list to read, and it works the same whichever
device you happen to be holding. Simultaneous confirms take the same lower-MAC tiebreak as §3.

**The confirming device does not move.** Its position *is* the choice, which is what makes the
gesture "pick a device" rather than "answer a question". This is worth asserting in the harness as
much as the seek is: a device that guessed a side would silently throw away the position the other
user was about to pick.

**The prompt owns Confirm and Back, and no other button.** The settings prompt of §5.1 holds every
button — defensible there, because the pair is genuinely broken and a page turn would be applied to
a dead layout. A divergent join is not that: both devices are reading correctly, just not together.
Holding every button would stop the reader dead until someone answered a prompt that has no
timeout, in the ordinary case of one device resumed and the other opened fresh. Turning a page
answers the prompt by reading on, and the prompt comes down — on the peer's turns as well as this
device's, since the sentence it displays is about a page nobody is looking at any more.

It shares the state machine with the force-sync prompt rather than growing one of its own. Both are
prompt-on-both / confirm-on-one with a lower-MAC tiebreak; two prompt machines racing for Confirm
on one screen is a bug waiting to be written.

### 4.4 Rotation re-triggers negotiation

`HalTiltSensor` can rotate one device mid-session, changing its viewport and therefore its
`compatHash` (§5). Rotation does **not** pause the pair or drop it to solo — it re-runs the §4.2
negotiation from the top. Same code path as a fresh join, so there is no separate recovery mode to
design or test: a layout change already re-greets, and a greeting starts a join round.

**Known, not yet handled: a wide re-pagination can prompt.** Both halves re-anchor on their content
offsets, so a rotation that leaves them on the same page self-heals as Identical and one that
leaves them a page apart as Aligned. But a re-pagination that moves them *further* than one page
apart has both answering NotAdjacent, which is Divergent — a resume prompt after an ordinary
rotation. The prompt is dismissible and a page turn takes it down (§4.3), so this is a nuisance
rather than a trap, and it is written down here so it is recognised rather than rediscovered on
hardware.

---

## 5. Compatibility hash — derived from `ReaderRenderSpec`, not hand-enumerated

Identical firmware does not guarantee identical layout: SD-installed fonts differ per device,
`HalTiltSensor` rotates each device independently, and font size / spacing / margins are all
per-device settings.

The codebase already has the authoritative answer to "what affects layout":
[`ReaderRenderSpec`](../lib/Epub/Epub/ReaderRenderSpec.h), whose own doc comment states
*"Section-cache validation keys on every field: a section file built with a different spec is
discarded and rebuilt."*

```cpp
struct ReaderRenderSpec {
  int fontId;  bool extraParagraphSpacing;  uint8_t paragraphAlignment;
  float lineCompression;   uint16_t viewportWidth, viewportHeight;
  bool hyphenationEnabled; bool embeddedStyle;  uint8_t imageRendering;
  bool focusReadingEnabled;
};
```

**`compatHash` = hash(`ReaderRenderSpec` ⊕ `bookId` ⊕ `SECTION_FILE_VERSION`).** Deriving it from
this struct rather than a hand-written list means it is complete by construction and stays
complete: anyone adding a render setting must add it here or the cache breaks, and the hash picks
it up for free. Note `viewportWidth/Height` already fold in orientation *and* margins, and
`lineCompression` folds in font family + line spacing.

Implemented: [PageFlipCompat](../lib/PageFlip/PageFlipCompat.h), FNV-1a, fields mixed a byte at a
time little-end first (hashing the struct's raw bytes would fold in padding, which is not identical
across compilers and would make two devices disagree for no reason). `SECTION_FILE_VERSION` is
exposed as `Section::FILE_VERSION` so the number keeps one home rather than being copied.

> **"Complete by construction" needs a mechanism — it is not free.** The claim above is true of the
> *cache*, which compares fields explicitly, but a hand-enumerated hash silently omits any field
> someone adds later. The obvious guard, `static_assert(sizeof(ReaderRenderSpec) == N)`, **does not
> work**: the struct is 18 bytes of members padded to 20, so two more `bool` fields fit inside the
> existing padding without changing its size, and would be missed. What does work is destructuring
> the spec with a structured binding and hashing the resulting names — a binding must name every
> member, so an eleventh field is a hard compile error regardless of padding. It is not a separate
> tripwire that can drift from the hash; it *is* the hash's field source.

### `fontId` is already portable — hash it as-is

An earlier draft of this document claimed `fontId` was a local, non-portable handle needing
substitution. **That was wrong.** Both kinds of font id are portable:

- **Built-in fonts** use stable compile-time IDs (`NOTOSERIF_14_FONT_ID`,
  [main.cpp:242-251](../src/main.cpp)) — identical across devices on the same firmware build.
- **SD-card fonts** are content-derived, not registration-ordered.
  `SdCardFontManager::computeFontId()`
  ([SdCardFontManager.cpp:18-29](../lib/EpdFont/SdCardFontManager.cpp)) is FNV-1a seeded with the
  font file's own `contentHash()`, then folded with family name and point size:

```cpp
uint32_t hash = contentHash;              // from the font file itself
while (*familyName) { hash ^= *familyName++; hash *= FNV_PRIME; }
hash ^= pointSize; hash *= FNV_PRIME;
```

Its own comment says the id "changes when font content changes (different header/TOC = different
contentHash)". So two devices holding the *same* font file compute the *same* id, and two devices
holding *different* files under the same family name compute *different* ids — which is exactly the
property a cross-device compat hash needs, and strictly better than the `(name, size, fileSize)`
substitute the earlier draft proposed.

**Hash `ReaderRenderSpec` verbatim, `fontId` included.** No substitution.

Two caveats worth knowing, neither of which changes the conclusion:

- Built-in ids are stable *for a given firmware build*. Two devices on different builds could in
  principle renumber them — but pair mode already assumes matching firmware.
- SD font registration has a collision guard that **skips** a font whose id collides
  ([SdCardFontManager.cpp:47-51](../lib/EpdFont/SdCardFontManager.cpp)), in which case that device
  resolves to the `0` "not found" sentinel. Astronomically unlikely, and the compat hash catches it
  as a mismatch rather than silently mis-rendering.

The hash rides in every packet (4 bytes) and is **recomputed on orientation change**, not just at
pair time — that is what makes §4.4 fire.

### Three things the implementation had to settle that this section did not

**The viewport is a `render()` output, so the hash cannot be built when the link comes up.**
`buildViewportWidth/Height` are captured inside `render()` and are zero until the first page has
been laid out. So `pageflipBegin()` sets the hash to a `0` sentinel and sends **no greeting at
all**: a hello advertising zero is a claim about a layout the device has not decided on, and would
read as a mismatch to any peer that had already rendered. The first greeting goes out from
`pageflipRefreshCompat()`, on the first pump where the viewport is known. Recomputing every pump —
rather than hooking individual settings — is also what makes §4.4 fire for free, and it covers more
than orientation: the automatic-page-turn toggle moves the bottom margin, so it genuinely
re-paginates and genuinely changes the hash.

**Zero is a sentinel and must be checked as one, on both sides.** Because the hash is unavailable
until the first render, a device is live on the wire advertising `0` for as long as that render
takes — seconds on a cold cache. A bare `peerHash == localHash` comparison reads that as a
*mismatch*, so two **identically configured** devices report incompatibility whenever one renders
faster than the other. That is not a corner case: it is "the second time you open the book on one
of the devices". `canCompareLayout()` therefore requires both hashes to be non-zero; an undecided
packet is neither agreement nor mismatch. The greeting is still answered so the handshake
completes, but nothing pairs on it, and both devices re-greet when their own first render lands, so
the pair converges a round later.

Neither pair harness could have caught this — both copy identical SD roots to the two halves, so
they render within a millisecond of each other and the skew is zero by construction.
`run_sim_pair_coldstart.sh` creates the skew deliberately (warm one cache, wipe the other) and
asserts neither half reports anything.

**Detection belongs on the greeting, not on the first turn.** `Hello` carries `compatHash`, so the
mismatch is caught before any position has been applied. An incompatible peer is still *answered*
— the answer carries this device's own hash, which is how the other user gets told too. One device
reporting the problem while the other silently does nothing is worse than either alone. The peer's
`turnSeq` is adopted either way: it is a session fact, not a layout one, and skipping it would
deadlock the pair at the moment §5.1 made them compatible.

**A peer that is present is not a peer that is paired.** This is the substance of the phase.
`pageflipPeerPresent` gates the two-step advance, and it must *not* be set by an incompatible peer:
leaving it set means this device turns two pages per press while the peer rejects every one of
them — the silent desync this section exists to prevent, dressed up as a working feature. Solo
reading is wrong-but-usable; that pair would be wrong-and-invisible. A mismatching peer likewise
does not refresh `lastPeerContactMs`, so it cannot hold the device awake through `preventAutoSleep`.

**Acting is immediate; telling the user waits.** Dropping the pairing happens on the packet, but the
notice is held for `MISMATCH_CONFIRM_MS` and cancelled if the layouts reconverge first. Two devices
in a shared case do not rotate in the same instant, so the one that turns first genuinely mismatches
for a moment before the other catches up — popping a notice on that transient would train the user
to ignore the notice that matters. The timer is drained by the pump rather than by the receive path,
because a *permanent* mismatch produces no further packets to hang the check on: the peer only
re-greets when its own hash changes.

Mismatch → do not sync silently. Offer §5.1.

### Outgoing positions must be read under the render lock

Every packet carries the page this device is showing, which means reading `section` — and `section`
belongs to the render task, which assigns and resets it inside `render()` while holding the lock.
Reading it from the pump unguarded is a use-after-free. It is not theoretical: the first version of
the compat refresh did exactly that, and because the first hash is computed during the *first*
render — the one building the very section it wants to describe — the left simulator half
segfaulted on essentially every run. It did not reproduce under `gdb`, which is the tell.

`pageflipSettledPage()` is the single guarded accessor: `RenderLock::peek()` first so the main task
never blocks behind a page render, then an actual `RenderLock` for the read itself, because peek
alone leaves the window open. Callers retry next pump. The greeting *answer* is latched into
`pageflipOweHelloAnswer` rather than sent inline, because a greeting is sent once — dropping the
answer because a render happened to be in flight would leave the peer waiting indefinitely.

---

## 5.1 Settings force-sync — making the hashes match

Detecting divergence is not enough: without a way to *fix* it, a mismatch is a dead end that reads
as "the feature is broken". The pair needs a deliberate "make these two devices match" action.

**Scope: only the settings that feed `ReaderRenderSpec`.** Not the whole settings store — button
mapping, sleep timeout, language and WiFi credentials are all legitimately per-device and must not
be clobbered.

| Synced | Feeds |
|---|---|
| `fontFamily` / `sdFontFamilyName`, font size | `fontId` |
| `lineSpacing` | `lineCompression` |
| `extraParagraphSpacing`, `paragraphAlignment` | direct |
| `screenMargin` | `viewportWidth/Height` |
| `hyphenationEnabled`, `embeddedStyle`, `imageRendering`, `focusReadingEnabled` | direct |

### That table is not the whole of what moves the viewport — and the rest is deliberately left out

`screenMargin` is only one of three inputs. The reader's viewport
([`ReaderUtils::readerLayoutBox()`](../src/activities/reader/ReaderUtils.h)) is the screen minus the
physical bezel, the user's margin, **the status bar** — `UITheme::getStatusBarHeight()` is computed
from `SETTINGS.statusBarSpec()`, so the clock, the battery percentage, the title and the progress
bar's thickness all move it — and **the automatic page-turn reservation**, which is runtime state
rather than a setting at all.

None of those are pushed. The pair shares a *layout*, not a device configuration, and a force-sync
that silently rewrote the other user's status bar would be doing something they did not ask for. So
a pair that differs there cannot be repaired by this action, and the honest outcome is an abort that
says which screens to go and match. That is a real, reachable case, and
`run_sim_pair_forcesync_abort.sh` is built on it.

### `orientation` is deliberately not in the table either

**Correction to an earlier draft.** It argued that pushing orientation would create a feedback loop,
because `HalTiltSensor` "drives orientation live, per device". That is wrong, and the citation
proves the opposite of what it was used for: `halTiltSensor.update(SETTINGS.tiltPageTurn,
SETTINGS.orientation, ...)` ([main.cpp:466](../src/main.cpp)) *reads* the orientation in order to
interpret a tilt **page-turn gesture** relative to the screen. There is no auto-rotation in this
firmware — nothing writes `SETTINGS.orientation` except an explicit user action — so there is no
loop to fear, and the follow-on requirement that a pair "agree on the tilt setting itself" does not
exist: `tiltPageTurn` has no effect on layout whatsoever.

Orientation still stays out, for the reason that survives: it describes how a device is being held,
which is physical and per-device. Two devices in a shared case are already oriented alike; two that
are not want rotating, not overwriting. It reaches the user through the same viewport abort above.

**Direction: same interaction as §4.3** — prompt on both, and confirming on one pushes *that*
device's render settings to the other. One consistent gesture for "this device is the source of
truth", used for both progress and settings.

The confirmed mismatch of §5 **is** that prompt, rather than a notice followed by a separate one:
reporting a problem and then offering nothing is the dead end this section opens by rejecting.

### Fonts are out of band: missing font is a hard error

**PageFlip never transfers font files.** Fonts are installed separately through the existing paths
(`FontInstaller`, the web server, or straight onto the SD card). If a font is missing on the peer,
force-sync **fails and says so** — it does not fall back, substitute, or transfer.

> **Correction to an earlier cost estimate.** A previous draft argued against font transfer partly
> on the grounds that chunked transfer "plus resume/verify machinery" would be a feature in its own
> right. That was wrong: `freeink::nearby::ReliableTransferSession` already provides chunked,
> sequenced, CRC32-verified transfer, and the SDK's `Offer`/`Accept`/`Data`/`Ack`/`Complete` packet
> types exist for exactly this. Transfer would be *much* cheaper than stated. The decision to keep
> fonts out of band still stands on its own merits — SD/web installation already works, and a
> multi-megabyte radio transfer on a 380KB device earns nothing the user cannot get faster another
> way — but it should stand for those reasons, not the mistaken cost estimate.

This has to be enforced by a **preflight, before anything is written**, because the existing
missing-font behaviour would otherwise hide the failure. At boot, `SdCardFontSystem::begin()`
resolves the saved family and, if the card does not have it, *silently clears the setting*
([SdCardFontSystem.cpp](../src/SdCardFontSystem.cpp)):

```cpp
} else {
  LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
  SETTINGS.clearSdFontFamily();
}
```

So an apply-then-discover sequence would leave the receiving device with its layout cache
invalidated *and* its font setting quietly reverted to a built-in — rendering in the wrong font,
with a hash that still mismatches. Exactly the silent desync §5 exists to prevent.

**Three-message force-sync** — offer, answer, apply:

```
1. OFFER    source broadcasts its render settings + the compatHash they produce
2. ANSWER   peer preflights and replies with the compatHash it WOULD have after applying
            source compares that against its own; anything but equal aborts, writing nothing
3. APPLY    only on a converging answer -- the commit is what turns a preflight into a change
```

### The answer is a hash, not a yes

**Correction to an earlier draft**, which had the peer answer `registry.findFamily(name) != nullptr`.
That check is necessary and *not sufficient*, in two ways that both end with a force-sync reporting
success while the pair stays broken — having invalidated a layout cache and rebuilt a whole chapter
to get there:

- **Same family name, different file.** `fontId` is computed from the font file's own
  `contentHash()` ([SdCardFontManager.cpp:44](../src/SdCardFontManager.cpp)), so two cards carrying
  different builds of "Bookerly" both answer yes and still hash differently.
- **The viewport is not about fonts at all.** Orientation, the status bar and the auto-page-turn
  reservation all move it, and none of them are pushed (above), so a peer can apply every offered
  field faithfully and still lay the page out differently.

Answering with the would-be hash folds in every input by construction — the same property §5 relies
on for the hash itself. It is computed by running `computePageFlipCompatHash()` over the
`ReaderRenderSpec` the peer *would* build, which is why the derivations behind that spec take their
inputs explicitly rather than reading the live settings.

`findFamily()` keeps its place as the first check, because it is the one whose failure the rest of
the machinery would otherwise hide (the silent clear above). The reason codes it and the component
comparisons produce — `MissingFont`, `FontDiffers`, `ScreenDiffers` — do not decide anything; the
hash comparison does. They exist so the abort can name the thing to fix and the device to fix it on:
*"Install font Bookerly on the right device"*, *"Match the rotation and status bar first"*.

A peer that answers `Ok` while landing on a different hash is therefore still refused. That is not a
hypothetical: two devices on different firmware builds lay identical settings out differently, and
`Section::FILE_VERSION` is in the hash precisely to catch it.

### Applying a synced setting is expensive — surface it

Any change to these fields invalidates the layout cache: the spec comparison at
[Section.cpp:167](../lib/Epub/Epub/Section.cpp) discards and rebuilds every `.bin`. For a large
book that is a long rebuild, so force-sync must be an explicit, confirmed action with progress —
never a silent background apply.

One real mitigation, from the comment at [Section.cpp:273-276](../lib/Epub/Epub/Section.cpp): the
unzipped HTML is keyed on the *book*, not on render settings, so it survives layout invalidation
and rebuilds "skip zip inflation entirely". The re-layout is therefore substantially cheaper than a
cold first open — worth telling the user so the wait is not mistaken for a hang.

Settings writes must keep the existing value-change guard (`if (newVal == _current) return;`) so a
force-sync that changes nothing does not burn a SPIFFS erase cycle. A push that changes nothing also
skips the rebuild entirely — if the pair already agreed on every pushed field, the difference is in
something this action does not carry, and re-laying out the book would not fix it.

### The applied page number is worthless — re-anchor on the offset

§4.2's rule applies to the receiving device's *own* saved position, not just to the join: the apply
invalidates the layout its page number counted. Applying therefore takes the same route the
text-settings screen already takes — remember the content offset, drop the section under
`RenderLock`, and let `getPageForVisibleTextOffset()` re-derive the page under the new layout.
Skipping that lands the user on an arbitrary page immediately after the action meant to fix the
desync.

Loading the new font belongs inside that same lock: `SdCardFontSystem::ensureLoaded()` frees the
resident `SdCardFont` that the render task may be walking. The `saveToFile()` goes outside it, so an
SD write never stalls a render.

---

## 6. WiFi coexistence — a real constraint

ESP-NOW peers must be on the **same channel**, and associating as STA lets the AP dictate the
channel. The firmware turns WiFi on for the web server, OTA, KOReader/OPDS sync and font
downloads.

Policy: **PageFlip suspends whenever WiFi STA/AP is active**, and resumes (re-pinning the
channel) when it returns to `WIFI_MODE_NULL`. Show the suspension in the UI rather than failing
silently.

---

## 7. Power — the honest unknown

`LOW_POWER_FREQ = 10` MHz on X4 (`#if BOARD_HAS_PSRAM` → 80, else 10;
[HalPowerManager.h:29-33](../lib/hal/HalPowerManager.h)) after 3s idle is the battery strategy.
The code already force-disables it whenever a radio is up
([HalPowerManager.cpp:30-31](../lib/hal/HalPowerManager.cpp)):

```cpp
auto wifiMode = WiFi.getMode();
if (wifiMode != WIFI_MODE_NULL) {
  enabled = false;  // Wifi is active, force disabling power saving
}
```

Note `CONFIG_PM_ENABLE` is **not set**, so IDF's automatic power-management locks are absent —
that hand-written check is the *only* thing protecting a live radio from the downclock.

**This is the measurement that decides the feature.** Whether duty-cycled ESP-NOW RX can coexist
with the 10 MHz idle is empirical; I have not measured it and would not guess. Three outcomes:

1. It survives → cost is just the RX duty cycle.
2. It needs 80 MHz only during the wake window → tolerable.
3. It needs 80 MHz continuously → the pair drains materially faster than a solo device, and that
   becomes a documented trade-off the user opts into.

### The SDK transport currently forces outcome 3 — by construction

`EspNowTransport::begin()`
([NearbyTransfer.cpp:133-145](../freeink-sdk/libs/network/NearbyTransfer/src/NearbyTransfer.cpp))
disables every power-saving mechanism this design depends on:

```cpp
WiFi.mode(WIFI_STA);
WiFi.disconnect(false);
WiFi.setSleep(false);
esp_wifi_set_channel(channel_, WIFI_SECOND_CHAN_NONE);
esp_wifi_set_ps(WIFI_PS_NONE);      // <-- no WiFi power save at all
esp_now_init();
```

Two consequences, both concrete rather than speculative:

- `WiFi.mode(WIFI_STA)` alone trips the guard at HalPowerManager.cpp:30-31, so simply *starting*
  the transport pins the CPU at full clock — the 10 MHz idle never engages.
- `WIFI_PS_NONE` means the radio never sleeps, so the §7 wake-window APIs have no effect while it
  stands.

This is correct for the SDK's intended use (bulk file transfer, where you want the radio hot), and
wrong for ours. PageFlip must, **after** `begin()` succeeds, re-enable modem sleep and install the
wake window/interval, then narrow the HalPowerManager guard so an ESP-NOW-only session is not
treated as "WiFi is active" for downclock purposes.

That narrowing is the actual step-1 experiment. It is a small change, but it is a change to shared
power-management behaviour, so it needs measuring rather than assuming — which is exactly what §7
was already gating on.

### Two-tier duty cycle

The mitigation that makes outcome 3 survivable: **you spend 30–60s reading a page and
milliseconds turning it.** The radio only needs to be responsive while you are actively turning.

| Tier | Window / interval | When |
|---|---|---|
| **Active** | 40ms / 400ms (~10% duty) | normal reading |
| **Deep idle** | 20ms / 2s (~1% duty) | after ~2 min with no turn |

A turn every 30–60s never drops out of Active, so normal reading is unaffected. Putting the book
down drops both devices to Deep idle. The next press ramps both back up, costing **one** slow page
turn (~1s) — acceptable, and it only happens when resuming after a pause.

Deep idle is a *listen* tier, not off. Both radios must stay reachable or neither can wake the
other; a fully-off idle state cannot work, because the pressing device's packet would land on a
deaf peer.

Sender retries across the receiver's window using the ESP-NOW TX-ACK callback.

---

## 8. Integration points

| Concern | Location |
|---|---|
| Transport | ✅ `PageFlipEspNowTransport` wraps `freeink::nearby::EspNowTransport`; `NearbyTransfer` symlinked in `platformio.ini`. `makePageFlipTransport()` picks it or the UDP shim by platform |
| Protocol + policy | new `lib/PageFlip/` |
| Per-frame pump | `loop()` after `gpio.update()`, [main.cpp:465](../src/main.cpp) |
| Local press: advance 2, broadcast | `EpubReaderActivity::pageTurn()` / `advanceOnePage()`, line 1040 |
| Receive: advance 2 or heal | reuse `pendingPageJump` (lines 1316-1318) under `RenderLock` (1051) |
| Radio gating by activity | mirror `bluetoothShouldBeActive()` / `bluetoothStartDeferred()` from `feat-bluetooth`'s `ActivityManager` |
| Auto-sleep coupling | `lastActivityTime`, main.cpp:494-499 |
| Compat hash source | `CrossPointSettings::readerRenderSpec()`, [CrossPointSettings.cpp:251-265](../src/CrossPointSettings.cpp) |
| Settings force-sync | the `ReaderRenderSpec`-feeding subset only (§5.1), via `JsonSettingsIO` |
| Settings | `pageflipEnabled`, `pageflipRole`, `pageflipPeerMac[6]` in `CrossPointSettings` + `JsonSettingsIO` |
| Build gate | `-DFREEINK_CAP_PAGEFLIP=1`, with stubs so it compiles out cleanly. Note master has only `FREEINK_DEVICE_*` (platformio.ini:155-202) — the `FREEINK_CAP_*` capability convention comes from `feat-bluetooth`, so we would be adopting it, not following it |

---

## 9. Phasing

1. **Transport spike** — `lib/PageFlip` pair + send/recv, no reader integration. Two devices,
   measure round-trip latency and idle current. *Gate: does §7 hold?*
2. ✅ **Position protocol** — `advanceOnePage()`, the deferred second step, both sides advance 2,
   `turnSeq` dedup. Assumes same book and matching settings. Driven by two simulator instances:
   `run_sim_pair.sh`.

   **What this does not yet do: the two devices move in lockstep, showing the same page rather than
   *P* and *P+1*.** The one-page offset is established by the join negotiation (§4.2), which is step
   5 — role only feeds the heal offset until then. The pair test asserts the halves stay
   byte-identical, and that assertion is a canary: when step 5 lands it must start failing, and be
   replaced by "differs by exactly one page".
3. ✅ **Compat hash** — §5, hashed from `ReaderRenderSpec` verbatim. Detect and report mismatch only,
   no repair yet: a one-shot popup, and the pair drops back to solo reading. Covered by
   `test/pageflip_compat` (per-field coverage plus a pinned wire value) and by
   `run_sim_pair_mismatch.sh`, the negative twin of the pair harness — it starts the right half with
   a different `screenMargin` and asserts both halves report it, neither counts the other as
   present, and the right half's page never moves under the left half's presses. Plus
   `run_sim_pair_coldstart.sh` for the skew case the symmetric harnesses cannot see.
4. ✅ **Settings force-sync** — §5.1. The prompt-on-both / confirm-on-one gesture, the
   `ReaderRenderSpec`-feeding subset, the preflight/abort, and the re-anchored rebuild. The
   preflight answers with a hash rather than a boolean, for the reasons recorded in §5.1. Covered by
   `test/pageflip_packet` and `test/pageflip_session` on the wire and the exchange, and by two pair
   harnesses: `run_sim_pair_forcesync.sh`, where the offer converges and the halves render alike
   again, and `run_sim_pair_forcesync_abort.sh`, where the divergence is a status-bar setting the
   push does not carry and the peer's `settings.json` must come out byte-identical.

   The rebuild wait deliberately reuses the existing indexing popup rather than growing a progress
   UI of its own: a settings-driven re-layout already looks like this everywhere else in the reader,
   and a second treatment for the same wait would be a new thing to explain.
5. ✅ **Lifecycle** — §4.1–4.4: own-page persistence, join negotiation with the classification,
   resume prompt, rotation re-negotiation, peer-offline indicator. This is where the pair stops
   being a mirror and becomes a spread.

   §4.1 needed no code: each device already persists `section->currentPage` plus its own content
   offset, and `applyDeferredReposition()` lands it on reopen. The negotiation gained a fourth case
   the design did not have (**Identical**, above) — it is the bootstrap, and without it the pair
   never acquires the one-page offset at all. §4.4 fell out for free: a layout change already
   re-greets, and a greeting starts a join round, so rotation re-negotiates with no recovery mode of
   its own. The peer-offline indicator turned out to need a **heartbeat** to be implementable at all
   (§4, presence expiry).

   The pair harnesses' "the halves are byte-identical" assertion was the canary step 2 planted, and
   it fired. It is replaced by something a straight left-versus-right comparison could not express —
   "different" is equally true of a pair one page apart and a pair that has desynced by nine. Each
   harness now walks a **reference instance with no peer**, one press per page, and measures every
   screenshot against it: `run_sim_pair.sh` asserts left on pages 0, 2, 4 and right on 1, 3, 5, and
   `run_sim_pair_forcesync.sh` asserts the repaired peer paginates like the source, one page behind.
   Two new harnesses cover the rest: `run_sim_pair_resume.sh` (divergent — and it checks that the
   confirming device does *not* move) and `run_sim_pair_offline.sh` (the press after the peer is
   gone must turn exactly one page).
6. **Coexistence** — §6 suspend/resume around WiFi.
7. **Power tuning** — wake window sweep against measured battery.
8. **UX polish** — pairing activity, settings entries, status indicator.

Steps 2, 4 and 5 are the bulk of the work and are where the fiddly bugs live. Note step 4 lands
before step 5 deliberately: the join negotiation in §4.2 assumes both devices already agree on
layout, so force-sync has to exist before rejoin logic is worth testing.

### Simulator note

Protocol, boundary logic, negotiation and force-sync (steps 2–5) can be developed **without
hardware** by putting a transport shim behind the `lib/PageFlip` interface — UDP loopback between
two simulator instances instead of ESP-NOW. The `sim-test` ScriptDriver can then drive both. Only
steps 1, 6 and 7 genuinely need two X4s.

Each instance takes a **slot**, set by environment variable, which fixes both the UDP port it
listens on and the synthetic MAC it reports:

```bash
CROSSPOINT_PAGEFLIP_SLOT=0 ./program --script left.script    # binds 127.0.0.1:47190
CROSSPOINT_PAGEFLIP_SLOT=1 ./program --script right.script   # binds 127.0.0.1:47191
```

`CROSSPOINT_PAGEFLIP_SLOTS` (default 2) and `CROSSPOINT_PAGEFLIP_PORT` (default 47190) override the
count and base port. `broadcast()` sends to every slot but its own, because ESP-NOW does not loop a
broadcast back to its sender and the protocol should not carry self-filtering that exists only for
the shim. The MAC is `02:50:46:00:00:<slot>` — locally administered, stable across runs, and
ordered by slot, so the lower-MAC tiebreak resolves identically on every scripted run.

Two implementation notes worth not rediscovering:

- **No `SO_REUSEADDR`.** UDP has no `TIME_WAIT`, so it buys nothing, and it would let two instances
  launched with the *same* slot both bind the port — after which the kernel delivers each datagram
  to only one of them and the pair looks intermittently deaf. Without it the second bind fails
  loudly, which is the diagnosis worth having. (This was a real bug; the duplicate-slot unit test
  is what caught it.)
- **`recvfrom` uses `MSG_TRUNC`** so an oversized datagram is dropped whole rather than delivered as
  its prefix. A truncated packet that still passed the length check would be a position applied from
  half a message.

---

## 10. Fork vs branch

Out of scope upstream — `SCOPE.md:58` lists **Active Connectivity** under Out-of-Scope: *"No RSS
readers, news aggregators, or web browsers. Background Wi-Fi drains the battery and complicates the
single-core CPU."* But the change is additive: one lib, one pairing activity, a
few settings, plus the `advanceFrom()` extraction — and the transport itself is already SDK code.
That is a **branch behind a build flag**, not a
hard fork — worth keeping rebaseable rather than repeating the port-not-merge problem.

---

## 11. Transport abstraction — and the wired escape hatch

`lib/PageFlip` exposes a transport interface with three implementations behind it:

| Transport | Purpose |
|---|---|
| **ESP-NOW** | the real thing — a thin wrapper over `freeink::nearby::EspNowTransport`, not a new implementation. Implemented: [PageFlipEspNowTransport](../lib/PageFlip/PageFlipEspNowTransport.h). Compiles and links on device; **unmeasured on hardware** |
| **UDP loopback** | two simulator instances. **Mandatory**, not a convenience: the SDK transport's `begin()` returns `false` under `SIMULATOR`. Implemented: [PageFlipUdpTransport](../lib/PageFlip/PageFlipUdpTransport.h) |
| **UART** | escape hatch if §7 fails |

The abstraction is not speculative — the simulator shim needs it on day one regardless. But it
earns its keep twice, because it makes the wired fallback cheap.

For a two-page spread the devices must be physically adjacent, almost certainly in a shared case.
A short UART link would be near-zero power, need no pairing, and sidestep §4, §6 and §7 entirely.
It is not the product answer — the whole appeal is two devices that also work standalone — but if
the power measurement in step 1 comes back at outcome 3, this is the fallback, and the transport
interface means taking it costs one implementation rather than a redesign.

---

## 12. Open questions

- **Power (§7)** — the one blocking unknown; decides the feature. Measure in step 1.
- **Does the book itself need to match by content, or just by path hash?** `bookId` is the existing
  path hash, so two devices with the *same filename but different EPUB files* would hash-match and
  desync. A size or mtime component would close it cheaply. Probably worth doing; not yet specced.
- **Solo reading on a paired device.** §4.1 has each device persisting its own displayed page, so
  reading a book solo on the right-hand device resumes exactly where that device left off — correct
  in isolation, but it silently widens the gap the next join has to negotiate. Acceptable, but the
  divergent prompt (§4.3) will fire more often than users may expect.
- **Does force-sync apply to the *current* book only, or globally?** §5.1 syncs global render
  settings, which invalidates layout caches for **every** cached book, not just the open one. That
  may be a much larger rebuild than the user is agreeing to when they confirm the prompt.
