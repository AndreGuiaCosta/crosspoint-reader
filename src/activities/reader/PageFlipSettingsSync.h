#pragma once

#ifdef FREEINK_CAP_PAGEFLIP

#include <PageFlipRenderSettings.h>

#include <cstdint>

class GfxRenderer;
class SdCardFontRegistry;

// The device half of the settings force-sync (docs/pageflip.md section 5.1): reading this device's
// render settings, judging a peer's, and writing them.
//
// Kept out of the reader activity because none of it needs the reader -- it needs the settings
// store, the font registry and the screen geometry -- and because the preflight is the part most
// worth reading on its own.
namespace PageFlipSettingsSync {

// This device's current render settings, ready to offer. The viewport comes from the caller for the
// same reason ReaderRenderSpec takes it: it is a render() output, not a stored setting.
PageFlipRenderSettings collect(uint16_t viewportWidth, uint16_t viewportHeight);

struct Preflight {
  PageFlipSyncResult result = PageFlipSyncResult::Unknown;
  // The compatHash this device would have after applying. The source compares it against its own,
  // which is what makes the verdict trustworthy: "I have that font" can be true while the two
  // devices still lay out differently.
  uint32_t resultHash = 0;
};

// What applying `offer` would do here. Writes nothing -- that is the whole point of the step.
Preflight preflight(const PageFlipRenderSettings& offer, const SdCardFontRegistry& registry,
                    const GfxRenderer& renderer, bool autoPageTurnActive, uint32_t bookId);

// Writes the offer into the settings store, keeping the value-change guard on every field. Returns
// true when something actually changed, which is the caller's cue to reload fonts, rebuild the
// layout and persist -- and, when nothing changed, its cue to burn no SPIFFS erase cycle at all.
bool apply(const PageFlipRenderSettings& offer);

}  // namespace PageFlipSettingsSync

#endif  // FREEINK_CAP_PAGEFLIP
