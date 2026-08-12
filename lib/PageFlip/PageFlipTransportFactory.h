#pragma once

#include <memory>

#include "PageFlipPacket.h"
#include "PageFlipTransport.h"

// Picks the link this build talks over: ESP-NOW on device, UDP loopback on the host so two
// simulator instances can pair (docs/pageflip.md section 11). Returns nullptr only if the
// allocation fails -- a transport that cannot start is still returned, because begin() failing is
// the ordinary "reading solo" case and must not be confused with an error.
//
// One function rather than a compile-time typedef so the caller holds the interface and nothing
// above lib/PageFlip needs to know which link it got.
std::unique_ptr<PageFlipTransport> makePageFlipTransport();

// Which half of the spread this device is, until the pairing UI exists (section 9 step 8 owns the
// setting). On the host the UDP slot decides, so two simulator instances take opposite roles with
// no configuration; on device everything is Left for now, which is wrong for a real pair and is
// exactly why role belongs in settings rather than here.
PageFlipRole defaultPageFlipRole();
