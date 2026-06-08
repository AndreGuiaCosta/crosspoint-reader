#pragma once
#include <Epub.h>

#include <memory>
#include <string>

#include "ProgressMapper.h"  // For CrossPointPosition — shared between backends.

// Wire-format position for the `book_configs` table. We send xpointer +
// progress and leave the CFI `location` empty; the receiver falls back to
// xpointer for navigation.
struct ReadestPosition {
  std::string xpointer;     // /body/DocFragment[N]/.../p[M]/text().offset
  std::string location;     // CFI; push leaves empty, pull may carry one
  int progressCurrent = 0;  // 1-based, book-wide estimate
  int progressTotal = 0;    // book-wide estimate
};

// Maps between CrossPoint's `(spineIndex, pageNumber, totalPages,
// paragraphIndex)` and `(xpointer, location, progress[])`. `progress`
// values are book-wide estimates from current-spine byte-to-page density
// so the receiver renders a stable "X / Y" across chapters.
class ReadestProgressMapper {
 public:
  static ReadestPosition toReadest(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos);

  // currentSpineIndex / totalPagesInCurrentSpine let the result rescale
  // pages-per-spine across chapters with different densities, delegating to
  // ProgressMapper::toCrossPoint for its XHTML-streaming xpath fallback.
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>& epub, const ReadestPosition& rPos,
                                         int currentSpineIndex = -1, int totalPagesInCurrentSpine = 0);
};
