#pragma once

#ifdef FREEINK_CAP_PAGEFLIP

#include <WiFi.h>

// Whether WiFi holds the one radio (docs/pageflip.md section 6). Shared by the reader and the
// pairing screen, because both bring the ESP-NOW link up and both would do the same damage.
//
// THE INVARIANT, which is the whole reason this has a comment: ask it only with the PageFlip link
// DOWN. Bringing the link up puts the radio in WIFI_STA itself, so with a link running this
// function answers "yes, WiFi is active" about the pair's own transport -- and anything that acted
// on that would tear the link down and rebuild it on the next pump, forever.
//
// What it protects: the SDK transport's begin() runs its own end() first, and that end()
// disconnects WiFi and switches the mode off. Coming up while a download is running does not merely
// fail to pair, it kills the download.
inline bool pageflipWifiHoldsRadio() { return WiFi.getMode() != WIFI_MODE_NULL; }

#endif  // FREEINK_CAP_PAGEFLIP
