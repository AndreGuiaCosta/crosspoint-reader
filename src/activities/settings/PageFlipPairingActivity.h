#pragma once

#ifdef FREEINK_CAP_PAGEFLIP

#include <GfxRenderer.h>
#include <PageFlipMac.h>
#include <PageFlipPacket.h>  // PageFlipRole
#include <PageFlipTransport.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

// Choosing which device this one is paired with (docs/pageflip.md section 8.1).
//
// Both devices have to be on this screen at once, and that is the design rather than a limitation:
// beacons only go out from here, so a device can only be discovered by somebody who also asked to
// pair. Listing every reader that had ever transmitted would put "is that one mine?" on the user,
// with a hex string to answer it.
class PageFlipPairingActivity final : public Activity {
 public:
  explicit PageFlipPairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PageFlipPairing", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // A device heard from since this screen opened.
  struct Peer {
    uint8_t mac[PageFlipTransport::MAC_BYTES] = {};
    PageFlipRole role = PageFlipRole::Left;
    unsigned long lastSeenMs = 0;
  };

  // Six is well past the point where the list stops being a list a user reads. It is a fixed array
  // rather than a vector because this screen has no reason to touch the heap at all.
  static constexpr size_t MAX_PEERS = 6;
  // Often enough that a device appears while the user is still looking at the screen, rarely enough
  // that two devices beaconing at each other is nothing next to a page render.
  static constexpr unsigned long BEACON_INTERVAL_MS = 1000;
  // Six missed beacons. A device that was switched off must leave the list, or the user pairs with
  // something that is not there and the failure looks like the pairing rather than the device.
  static constexpr unsigned long PEER_EXPIRY_MS = BEACON_INTERVAL_MS * 6;
  // How long "now do the same on the other device" stays up before the screen closes itself.
  static constexpr unsigned long CONFIRM_NOTICE_MS = 2500;

  void sendBeacon();
  void receiveBeacons();
  void expirePeers();
  void handleSelection();
  // Row 0 is "any nearby device"; the peers follow it.
  int rowCount() const { return static_cast<int>(1 + peerCount); }
  bool isSelectedRow(int index) const;
  std::string rowTitle(int index) const;
  std::string rowSubtitle(int index) const;

  void onBack() { finish(); }

  std::unique_ptr<PageFlipTransport> transport;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  Peer peers[MAX_PEERS];
  size_t peerCount = 0;
  unsigned long lastBeaconMs = 0;

  // This device's own MAC, shown in the header so the two screens can be told apart. Without it the
  // user is choosing between hex strings with nothing to match them against.
  char localMacText[PageFlipMac::TEXT_LENGTH] = "";
  // WiFi holds the one radio (section 6), so there is nothing to discover and the screen says why
  // rather than showing an empty list forever.
  bool radioUnavailable = false;
  // Set when a choice has been made: the screen holds the "now pair the other one too" notice, then
  // closes. The warning belongs here rather than in the list, because it is only actionable once
  // the user has paired one device and is about to walk away.
  unsigned long confirmedAtMs = 0;
};

#endif  // FREEINK_CAP_PAGEFLIP
