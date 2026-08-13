#include "PageFlipSettingsSync.h"

#ifdef FREEINK_CAP_PAGEFLIP

#include <Epub/Section.h>
#include <PageFlipCompat.h>
#include <SdCardFontRegistry.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
// ReaderUtils.h reaches ActivityManager, which holds a unique_ptr<Activity>, so the full type has
// to be in scope before it -- every other consumer gets that from its own activity header.
#include "activities/Activity.h"
#include "ReaderUtils.h"

namespace PageFlipSettingsSync {

PageFlipRenderSettings collect(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  PageFlipRenderSettings settings;
  std::snprintf(settings.sdFontFamilyName, sizeof(settings.sdFontFamilyName), "%s", SETTINGS.sdFontFamilyName);
  // Derived, and carried only so the peer can name what differs -- see PageFlipRenderSettings.
  settings.fontId = SETTINGS.getReaderFontId();
  settings.viewportWidth = viewportWidth;
  settings.viewportHeight = viewportHeight;

  settings.fontFamily = SETTINGS.fontFamily;
  settings.fontPointSize = SETTINGS.fontPointSize;
  settings.lineSpacing = SETTINGS.lineSpacing;
  settings.paragraphAlignment = SETTINGS.paragraphAlignment;
  settings.screenMargin = SETTINGS.screenMargin;
  settings.imageRendering = SETTINGS.imageRendering;
  settings.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  settings.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  settings.embeddedStyle = SETTINGS.embeddedStyle;
  settings.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  return settings;
}

Preflight preflight(const PageFlipRenderSettings& offer, const SdCardFontRegistry& registry,
                    const GfxRenderer& renderer, const bool autoPageTurnActive, const uint32_t bookId) {
  // The font first, because its failure is the one the rest of the machinery would hide.
  // SdCardFontSystem::begin() silently clears a family the card does not have, so applying and then
  // discovering would leave this device with an invalidated layout cache, a reverted font setting,
  // and a hash that still mismatches -- the exact silent desync section 5 exists to prevent.
  const bool wantsSdFont = offer.sdFontFamilyName[0] != '\0';
  if (wantsSdFont && registry.findFamily(offer.sdFontFamilyName) == nullptr) {
    return {PageFlipSyncResult::MissingFont, 0};
  }

  // Everything below is what this device *would* resolve, asked of the same code that will do the
  // resolving for real. Both derivations take their inputs explicitly for that reason.
  const int candidateFontId = SETTINGS.readerFontIdFor(offer.sdFontFamilyName, offer.fontFamily, offer.fontPointSize);
  const ReaderUtils::ReaderLayoutBox box = ReaderUtils::readerLayoutBox(renderer, offer.screenMargin,
                                                                       autoPageTurnActive);

  ReaderRenderSpec spec;
  // Assembled through a structured binding for the same reason computePageFlipCompatHash() reads
  // one: a binding must name every member, so an eleventh field added to ReaderRenderSpec stops
  // compiling HERE too. This is the second place the struct gets filled by hand, and the guard over
  // in PageFlipCompat.cpp cannot see it -- a field left at its default here would leave the
  // would-be hash permanently unequal to the source's, aborting every force-sync with the generic
  // "could not be matched" and nothing pointing at why.
  auto& [fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
         hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled] = spec;
  fontId = candidateFontId;
  lineCompression = CrossPointSettings::readerLineCompressionFor(wantsSdFont, offer.fontFamily, offer.lineSpacing);
  extraParagraphSpacing = offer.extraParagraphSpacing != 0;
  paragraphAlignment = offer.paragraphAlignment;
  viewportWidth = box.viewportWidth;
  viewportHeight = box.viewportHeight;
  hyphenationEnabled = offer.hyphenationEnabled != 0;
  embeddedStyle = offer.embeddedStyle != 0;
  imageRendering = offer.imageRendering;
  focusReadingEnabled = offer.focusReadingEnabled != 0;

  Preflight verdict;
  verdict.resultHash = computePageFlipCompatHash(spec, bookId, Section::FILE_VERSION);

  // The reason codes are a diagnosis, not the decision -- the source's hash comparison is that.
  // Which is why an agreement on every component below can still be answered Ok and then rejected:
  // a different firmware build lays the same settings out differently, and only the hash sees it.
  if (candidateFontId != offer.fontId) {
    // The family resolved, so the card has *a* font by that name -- a different file for it. The id
    // comes from the file's own content hash, which is what catches this at all.
    verdict.result = PageFlipSyncResult::FontDiffers;
  } else if (box.viewportWidth != offer.viewportWidth || box.viewportHeight != offer.viewportHeight) {
    // Orientation is not pushed (section 5.1), and neither the status bar nor the automatic
    // page-turn reservation is part of the offer, so this is the honest verdict for all three.
    verdict.result = PageFlipSyncResult::ScreenDiffers;
  } else {
    verdict.result = PageFlipSyncResult::Ok;
  }
  return verdict;
}

bool apply(const PageFlipRenderSettings& offer) {
  bool changed = false;
  // Each write keeps its value-change guard: a force-sync that changes nothing must not cost a
  // SPIFFS erase cycle, and the caller uses the same answer to decide whether to rebuild at all.
  const auto write = [&changed](uint8_t& field, const uint8_t value) {
    if (field == value) return;
    field = value;
    changed = true;
  };

  write(SETTINGS.fontFamily, offer.fontFamily);
  write(SETTINGS.fontPointSize, offer.fontPointSize);
  write(SETTINGS.lineSpacing, offer.lineSpacing);
  write(SETTINGS.paragraphAlignment, offer.paragraphAlignment);
  write(SETTINGS.screenMargin, offer.screenMargin);
  write(SETTINGS.imageRendering, offer.imageRendering);
  write(SETTINGS.extraParagraphSpacing, offer.extraParagraphSpacing);
  write(SETTINGS.hyphenationEnabled, offer.hyphenationEnabled);
  write(SETTINGS.embeddedStyle, offer.embeddedStyle);
  write(SETTINGS.focusReadingEnabled, offer.focusReadingEnabled);

  if (std::strncmp(SETTINGS.sdFontFamilyName, offer.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName)) != 0) {
    std::snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", offer.sdFontFamilyName);
    changed = true;
  }

  // Deliberately not written: orientation, and everything that is not a layout input. The pair
  // shares a layout, not a device configuration.
  return changed;
}

}  // namespace PageFlipSettingsSync

#endif  // FREEINK_CAP_PAGEFLIP
