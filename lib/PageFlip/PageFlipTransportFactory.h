#pragma once

#include <memory>

#include "PageFlipTransport.h"

// Picks the link this build talks over: ESP-NOW on device, UDP loopback on the host so two
// simulator instances can pair (docs/pageflip.md section 11). Returns nullptr only if the
// allocation fails -- a transport that cannot start is still returned, because begin() failing is
// the ordinary "reading solo" case and must not be confused with an error.
//
// One function rather than a compile-time typedef so the caller holds the interface and nothing
// above lib/PageFlip needs to know which link it got.
std::unique_ptr<PageFlipTransport> makePageFlipTransport();
