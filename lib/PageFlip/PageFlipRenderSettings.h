#pragma once

#include <cstddef>
#include <cstdint>

// The settings force-sync payload (docs/pageflip.md section 5.1).
//
// Only the settings that feed ReaderRenderSpec travel: button mapping, sleep timeout, language and
// WiFi credentials are legitimately per-device and must never be clobbered by a pair action.
//
// Field names mirror CrossPointSettings so the mapping either side of the wire is obvious. The
// values are raw setting values, not derived ones -- the receiver has to write them back into its
// own store, and a derived value cannot be written back.
//
// fontId and the viewport are the exceptions: they are *derived*, carried for diagnosis only. The
// receiver recomputes both from its own device before applying anything (section 5.1's preflight),
// and comparing them against the sender's is what lets an abort name the cause -- "that device has
// a different file for this font" rather than "the hashes differ".
//
// Deliberately free of Arduino includes so the host unit tests build it directly.
struct PageFlipRenderSettings {
  // Matches CrossPointSettings::sdFontFamilyName, including its null terminator, so a name that
  // fits in the setting fits on the wire.
  static constexpr size_t FONT_NAME_CAPACITY = 32;
  static constexpr size_t FONT_NAME_MAX_LENGTH = FONT_NAME_CAPACITY - 1;

  // Empty means "use the built-in family below", the same convention the setting itself uses.
  char sdFontFamilyName[FONT_NAME_CAPACITY] = "";

  // Derived, diagnostic only -- see above.
  int32_t fontId = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;

  uint8_t fontFamily = 0;
  uint8_t fontPointSize = 0;
  uint8_t lineSpacing = 0;
  uint8_t paragraphAlignment = 0;
  uint8_t screenMargin = 0;
  uint8_t imageRendering = 0;
  uint8_t extraParagraphSpacing = 0;
  uint8_t hyphenationEnabled = 0;
  uint8_t embeddedStyle = 0;
  uint8_t focusReadingEnabled = 0;
};

// Why a force-sync did or did not converge, as judged by the device being pushed to.
//
// The preflight answers with the compatHash it *would* have after applying, and the source compares
// that against its own -- which folds in every input to the hash by construction, rather than
// trusting a list of checks to stay complete. These codes exist so the abort can say something the
// user can act on; they never decide the outcome, the hashes do.
enum class PageFlipSyncResult : uint8_t {
  Ok = 0,             // the peer would end up with an identical layout: safe to commit
  MissingFont = 1,    // the named SD font is not installed on the peer -- install it there
  FontDiffers = 2,    // same family name, different font file, so a different fontId
  ScreenDiffers = 3,  // the viewport would still differ: orientation, status bar, or margins
  Unknown = 4,        // hashes differ for a reason the components cannot name (e.g. a different build)
};
