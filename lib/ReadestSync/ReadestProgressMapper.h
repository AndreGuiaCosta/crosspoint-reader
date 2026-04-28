#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "ProgressMapper.h"  // For CrossPointPosition — shared between backends.

/**
 * Wire-format position for Readest's `book_configs` table (handoff §7).
 *
 * Readest's canonical position is `location` (an EPUB CFI), but it also
 * stores `xpointer` for KOReader interop and `progress` as `[current,
 * total]`. CrossPoint can produce CFI-equivalent xpointers cheaply
 * (same format CREngine uses), so V1 sends `xpointer` + `progress` and
 * leaves `location` empty per §7.3.
 */
struct ReadestPosition {
  std::string xpointer;     // /body/DocFragment[N]/.../p[M]/text().offset
  std::string location;     // CFI; V1 push leaves empty, pull may carry one
  int progressCurrent = 0;  // 1-based, book-wide estimate
  int progressTotal = 0;    // book-wide estimate
};

/**
 * Maps between CrossPoint's `(spineIndex, pageNumber, totalPages,
 * paragraphIndex)` and Readest's `(xpointer, location, progress[])`.
 *
 * The xpointer side reuses `ChapterXPathResolver` from `KOReaderSync` —
 * Readest's xpointer format and CREngine's are byte-identical (§7.2),
 * and there is no value in two implementations drifting apart.
 *
 * Page numbers in `progress` are book-wide estimates derived from the
 * current spine's byte-to-page density. The other Readest clients
 * (phone, web) display these as "X / Y" so a section-local count would
 * be confusing.
 */
class ReadestProgressMapper {
 public:
  static ReadestPosition toReadest(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  /**
   * @param epub                       The currently open book.
   * @param rPos                       Pulled position from the server.
   * @param currentSpineIndex          The spine the reader is on right now,
   *                                   used to scale pages-per-spine across
   *                                   chapters with different densities.
   * @param totalPagesInCurrentSpine   Pagination of the current spine.
   */
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const ReadestPosition& rPos,
                                         int currentSpineIndex = -1, int totalPagesInCurrentSpine = 0);
};
