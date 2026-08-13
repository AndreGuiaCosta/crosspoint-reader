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
#include "fontIds.h"

namespace {

constexpr const char* MODULE = "PFPAIR";

PageFlipRole configuredRole() {
  return SETTINGS.pageflipRole == CrossPointSettings::PAGEFLIP_ROLE_RIGHT ? PageFlipRole::Right : PageFlipRole::Left;
}

}  // namespace

void PageFlipPairingActivity::onEnter() {
  Activity::onEnter();

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
  Activity::onExit();
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
  // inside it or the next Confirm pairs with whatever slid into that row.
  if (selectedIndex >= rowCount()) selectedIndex = rowCount() - 1;
  requestUpdate();
}

bool PageFlipPairingActivity::isSelectedRow(const int index) const {
  uint8_t stored[PageFlipTransport::MAC_BYTES] = {};
  const bool paired = PageFlipMac::parse(SETTINGS.pageflipPeerMac, stored);
  if (index == 0) return !paired;
  if (!paired) return false;
  const size_t peer = static_cast<size_t>(index - 1);
  return peer < peerCount && std::memcmp(peers[peer].mac, stored, sizeof(stored)) == 0;
}

std::string PageFlipPairingActivity::rowTitle(const int index) const {
  if (index == 0) return tr(STR_PAGEFLIP_PAIR_ANY);
  const size_t peer = static_cast<size_t>(index - 1);
  if (peer >= peerCount) return "";
  char text[PageFlipMac::TEXT_LENGTH] = "";
  PageFlipMac::format(peers[peer].mac, text, sizeof(text));
  return text;
}

std::string PageFlipPairingActivity::rowSubtitle(const int index) const {
  if (index == 0) return tr(STR_PAGEFLIP_PAIR_ANY_HINT);
  const size_t peer = static_cast<size_t>(index - 1);
  if (peer >= peerCount) return "";
  // The other device's half, which is the one thing on this screen a user can check against the
  // room: two devices both calling themselves the left one is the setup error that leaves the pair
  // showing the same page, and it is visible here before it is visible in a book.
  return peers[peer].role == PageFlipRole::Left ? tr(STR_PAGEFLIP_DEVICE_LEFT_ROW)
                                                : tr(STR_PAGEFLIP_DEVICE_RIGHT_ROW);
}

void PageFlipPairingActivity::handleSelection() {
  if (selectedIndex == 0) {
    SETTINGS.pageflipPeerMac[0] = '\0';
    LOG_INF(MODULE, "Paired device cleared");
  } else {
    const size_t peer = static_cast<size_t>(selectedIndex - 1);
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

void PageFlipPairingActivity::loop() {
  if (confirmedAtMs != 0) {
    if (millis() - confirmedAtMs >= CONFIRM_NOTICE_MS) onBack();
    return;  // the choice is made; the screen is only finishing what it has to say about it
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (!radioUnavailable) {
    receiveBeacons();
    expirePeers();
    if (millis() - lastBeaconMs >= BEACON_INTERVAL_MS) sendBeacon();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int rows = rowCount();
  buttonNavigator.onNextRelease([this, rows] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, rows);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, rows] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, rows);
    requestUpdate();
  });
}

void PageFlipPairingActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  // The subtitle is this device's own MAC: the list on the other screen is a column of hex, and
  // this is what tells the user which line of it is them.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PAGEFLIP_PAIR),
                 radioUnavailable ? tr(STR_PAGEFLIP_WIFI_PAUSED) : localMacText);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, rowCount(), selectedIndex,
      [this](int index) { return rowTitle(index); }, [this](int index) { return rowSubtitle(index); }, nullptr,
      [this](int index) { return isSelectedRow(index) ? std::string(tr(STR_SELECTED)) : std::string(); }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // Last, so it sits over the list it is about.
  if (confirmedAtMs != 0) GUI.drawPopup(renderer, tr(STR_PAGEFLIP_PAIR_BOTH));

  renderer.displayBuffer();
}

#endif  // FREEINK_CAP_PAGEFLIP
