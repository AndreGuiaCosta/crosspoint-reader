#include "PageFlipPairingActivity.h"

#ifdef FREEINK_CAP_PAGEFLIP

#include <I18n.h>
#include <Logging.h>
#include <PageFlipPacket.h>
#include <PageFlipTransportFactory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "PageFlipRadio.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr const char* MODULE = "PFPAIR";

PageFlipRole configuredRole() {
  return SETTINGS.pageflipRole == CrossPointSettings::PAGEFLIP_ROLE_RIGHT ? PageFlipRole::Right : PageFlipRole::Left;
}

}  // namespace

void PageFlipPairingActivity::onEnter() {
  UiListActivity::onEnter();

  // Section 6, and the same rule the reader follows: asked only with the link down, which here is
  // every time, because this screen has just opened. Bringing ESP-NOW up runs the SDK transport's
  // end() first, and that disconnects WiFi -- pairing during a download would kill the download.
  radioUnavailable = pageflipWifiHoldsRadio();
  if (radioUnavailable) {
    LOG_INF(MODULE, "WiFi has the radio; nothing to discover");
    requestUpdate();
    return;
  }

  transport = makePageFlipTransport();
  if (!transport || !transport->begin()) {
    LOG_ERR(MODULE, "PageFlip transport unavailable");
    transport.reset();
    radioUnavailable = true;
    requestUpdate();
    return;
  }

  uint8_t mac[PageFlipTransport::MAC_BYTES] = {};
  if (transport->localMac(mac)) PageFlipMac::format(mac, localMacText, sizeof(localMacText));
  LOG_INF(MODULE, "Pairing screen up as %s, this device is %s",
          configuredRole() == PageFlipRole::Left ? "left" : "right", localMacText);

  sendBeacon();
  requestUpdate();
}

void PageFlipPairingActivity::onExit() {
  // Releases the radio, and the SDK's end() puts the WiFi mode back to off with it. Leaving it up
  // would mean the next book opened found the reader's own section 6 guard tripped by this screen.
  transport.reset();
  UiListActivity::onExit();
}

void PageFlipPairingActivity::sendBeacon() {
  if (!transport) return;
  uint8_t wire[PageFlipPacket::PAIR_BEACON_BYTES] = {};
  size_t length = 0;
  if (!PageFlipPacket::encodePairBeacon(configuredRole(), wire, sizeof(wire), length)) return;
  transport->broadcast(wire, length);
  lastBeaconMs = millis();
}

void PageFlipPairingActivity::receiveBeacons() {
  if (!transport) return;

  uint8_t buffer[PageFlipTransport::MAX_PAYLOAD_BYTES];
  size_t length = 0;
  uint8_t senderMac[PageFlipTransport::MAC_BYTES] = {};
  // Drained rather than polled once: a frame that took a while leaves several beacons queued, and
  // handling one per frame would let the queue outrun the screen.
  while (transport->poll(buffer, sizeof(buffer), length, senderMac)) {
    PageFlipRole role = PageFlipRole::Left;
    // Anything else on the channel -- a pair reading nearby, most likely -- is not a device asking
    // to be paired with, and must not appear in a list the user is about to choose from.
    if (!PageFlipPacket::decodePairBeacon(buffer, length, role)) continue;

    bool known = false;
    for (size_t i = 0; i < peerCount; ++i) {
      if (std::memcmp(peers[i].mac, senderMac, PageFlipTransport::MAC_BYTES) != 0) continue;
      // The role can change under us: the other user is one screen away from the role setting, and
      // may well be there because this screen told them the two halves matched.
      peers[i].role = role;
      peers[i].lastSeenMs = millis();
      known = true;
      break;
    }
    if (known || peerCount >= MAX_PEERS) continue;

    std::memcpy(peers[peerCount].mac, senderMac, PageFlipTransport::MAC_BYTES);
    PageFlipMac::format(peers[peerCount].mac, peers[peerCount].macText, sizeof(peers[peerCount].macText));
    peers[peerCount].role = role;
    peers[peerCount].lastSeenMs = millis();
    ++peerCount;
    LOG_INF(MODULE, "Found a %s device", role == PageFlipRole::Left ? "left" : "right");
    requestUpdate();
  }
}

void PageFlipPairingActivity::expirePeers() {
  const unsigned long now = millis();
  size_t kept = 0;
  for (size_t i = 0; i < peerCount; ++i) {
    if (now - peers[i].lastSeenMs >= PEER_EXPIRY_MS) continue;
    if (kept != i) peers[kept] = peers[i];
    ++kept;
  }
  if (kept == peerCount) return;

  peerCount = kept;
  // The selection is an index into a list that just got shorter, so it has to be brought back
  // inside it or the next Confirm pairs with whatever slid into that row. moveSelectionTo also
  // pulls the viewport with it, which a bare assignment would leave scrolled past the last row.
  if (nav.selected >= rowCount()) {
    moveSelectionTo(rowCount() - 1);
  } else {
    requestUpdate();
  }
}

bool PageFlipPairingActivity::isSelectedRow(const int index) const {
  uint8_t stored[PageFlipTransport::MAC_BYTES] = {};
  const bool paired = PageFlipMac::parse(SETTINGS.pageflipPeerMac, stored);
  if (index == 0) return !paired;
  if (!paired) return false;
  const size_t peer = static_cast<size_t>(index - 1);
  return peer < peerCount && std::memcmp(peers[peer].mac, stored, sizeof(stored)) == 0;
}

void PageFlipPairingActivity::handleSelection() {
  if (nav.selected == 0) {
    SETTINGS.pageflipPeerMac[0] = '\0';
    LOG_INF(MODULE, "Paired device cleared");
  } else {
    const size_t peer = static_cast<size_t>(nav.selected - 1);
    if (peer >= peerCount) return;
    if (!PageFlipMac::format(peers[peer].mac, SETTINGS.pageflipPeerMac, sizeof(SETTINGS.pageflipPeerMac))) return;
    LOG_INF(MODULE, "Paired with %s", SETTINGS.pageflipPeerMac);
  }
  SETTINGS.saveToFile();

  // Held on screen rather than returning at once. Pairing is per device: this half now ignores
  // strangers and the other one still accepts anyone, the pair works either way, and nothing would
  // ever say so again.
  confirmedAtMs = millis();
  requestUpdate();
}

void PageFlipPairingActivity::activateIndex(const int index) {
  nav.selected = index;
  // A tap flash left behind would gray an unrelated row on the notice frame that follows.
  app.clearTapFlash();
  handleSelection();
}

void PageFlipPairingActivity::loop() {
  if (confirmedAtMs != 0) {
    if (millis() - confirmedAtMs >= CONFIRM_NOTICE_MS) finish();
    return;  // the choice is made; the screen is only finishing what it has to say about it
  }

  if (!radioUnavailable) {
    receiveBeacons();
    expirePeers();
    if (millis() - lastBeaconMs >= BEACON_INTERVAL_MS) sendBeacon();
  }

  UiListActivity::loop();
}

void PageFlipPairingActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the header band drawn by drawChrome(), above the button hints; derived from the
  // safe area so board bezel insets apply.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int rows = rowCount();
  // Rebuilt every pass rather than cached: the list is at most seven borrowed pointers, and a peer
  // can appear, change its half, or expire between any two frames.
  for (int i = 0; i < rows; ++i) {
    fui::ListItem& item = rowItems[i];
    item = fui::ListItem{};
    if (i == 0) {
      item.label = tr(STR_PAGEFLIP_PAIR_ANY);
      item.subtitle = tr(STR_PAGEFLIP_PAIR_ANY_HINT);
    } else {
      const size_t peer = static_cast<size_t>(i - 1);
      item.label = peers[peer].macText;
      // The other device's half, which is the one thing on this screen a user can check against the
      // room: two devices both calling themselves the left one is the setup error that leaves the
      // pair showing the same page, and it is visible here before it is visible in a book.
      item.subtitle = peers[peer].role == PageFlipRole::Left ? tr(STR_PAGEFLIP_DEVICE_LEFT_ROW)
                                                            : tr(STR_PAGEFLIP_DEVICE_RIGHT_ROW);
    }
    if (isSelectedRow(i)) item.value = tr(STR_SELECTED);
    item.actionValue = static_cast<int16_t>(i);
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rows);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void PageFlipPairingActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // The subtitle is this device's own MAC: the list on the other screen is a column of hex, and
  // this is what tells the user which line of it is them.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_PAGEFLIP_PAIR), radioUnavailable ? tr(STR_PAGEFLIP_WIFI_PAUSED) : localMacText);
}

void PageFlipPairingActivity::drawFooter() {
  UiListActivity::drawFooter();
  // Last, so it sits over the list it is about.
  if (confirmedAtMs != 0) GUI.drawPopup(renderer, tr(STR_PAGEFLIP_PAIR_BOTH));
}

#endif  // FREEINK_CAP_PAGEFLIP
