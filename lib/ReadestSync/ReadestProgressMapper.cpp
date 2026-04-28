#include "ReadestProgressMapper.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ChapterXPathResolver.h"

namespace {
// Parse the integer between `prefix` and the next `]` in `s`. Returns -1
// if the prefix isn't found or the contents aren't a positive decimal.
// `last == true` searches from the end (used to grab the deepest `/p[N]`).
int parseIndexAfter(const std::string& s, const char* prefix, bool last) {
  const size_t prefixLen = std::strlen(prefix);
  const size_t pos = last ? s.rfind(prefix) : s.find(prefix);
  if (pos == std::string::npos) return -1;
  const size_t numStart = pos + prefixLen;
  const size_t numEnd = s.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return -1;
  int v = 0;
  for (size_t i = numStart; i < numEnd; ++i) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

// Extract the second `/N` step from an `epubcfi(/A/B!/...)` string. Per
// spec §7.1, Readest's spine convention puts the spine step second
// (the leading step is `/6` for the spine collection); 0-based spine =
// (step - 2) / 2. Returns -1 on any parse failure.
int parseCfiSpineStep(const std::string& cfi) {
  const size_t open = cfi.find("epubcfi(");
  if (open == std::string::npos) return -1;
  size_t pos = cfi.find('/', open);
  if (pos == std::string::npos) return -1;
  pos = cfi.find('/', pos + 1);
  if (pos == std::string::npos) return -1;
  ++pos;
  int v = 0;
  bool any = false;
  while (pos < cfi.size() && cfi[pos] >= '0' && cfi[pos] <= '9') {
    v = v * 10 + (cfi[pos] - '0');
    ++pos;
    any = true;
  }
  return any ? v : -1;
}
}  // namespace

ReadestPosition ReadestProgressMapper::toReadest(const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos) {
  ReadestPosition r;
  if (!epub) return r;

  const float intra =
      (pos.totalPages > 0) ? static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages) : 0.0f;
  if (pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    r.xpointer = ChapterXPathResolver::findXPathForParagraph(epub, pos.spineIndex, pos.paragraphIndex);
  } else {
    r.xpointer = ChapterXPathResolver::findXPathForProgress(epub, pos.spineIndex, intra);
  }
  if (r.xpointer.empty()) {
    // Section-granularity fallback (§7.3): bare DocFragment is enough
    // for Readest to jump to the correct chapter on the other end.
    r.xpointer = "/body/DocFragment[" + std::to_string(pos.spineIndex + 1) + "]";
  }

  // V1 deliberately leaves CFI empty — see §7.3 ("do not send a malformed
  // CFI"). foliate-js on the receiving end accepts an empty string and
  // falls back to xpointer for navigation.
  r.location.clear();

  // Estimate book-wide pages from current-spine byte density. Section-local
  // numbers would surface as a confusing "X / Y" jumping at every chapter
  // boundary on phone/web Readest.
  const size_t bookSize = epub->getBookSize();
  if (bookSize > 0 && pos.totalPages > 0) {
    const size_t prevCum = (pos.spineIndex > 0) ? epub->getCumulativeSpineItemSize(pos.spineIndex - 1) : 0;
    const size_t spineSize = epub->getCumulativeSpineItemSize(pos.spineIndex) - prevCum;
    if (spineSize > 0) {
      const float density = static_cast<float>(pos.totalPages) / static_cast<float>(spineSize);
      r.progressTotal = std::max(1, static_cast<int>(std::round(bookSize * density)));
      const int currentEstimate =
          static_cast<int>(std::round(prevCum * density)) + std::max(0, pos.pageNumber) + 1;  // 1-based
      r.progressCurrent = std::clamp(currentEstimate, 1, r.progressTotal);
    }
  }
  if (r.progressTotal == 0) {
    // Density estimate failed (single-spine book, missing sizes, etc.).
    // Fall back to section-local 1-based pages — better than zeros.
    r.progressCurrent = std::max(1, pos.pageNumber + 1);
    r.progressTotal = std::max(r.progressCurrent, std::max(1, pos.totalPages));
  }

  LOG_DBG("RPM", "-> R: spine=%d page=%d/%d -> xpointer=%s progress=[%d,%d]", pos.spineIndex, pos.pageNumber,
          pos.totalPages, r.xpointer.c_str(), r.progressCurrent, r.progressTotal);
  return r;
}

CrossPointPosition ReadestProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const ReadestPosition& rPos,
                                                       int currentSpineIndex, int totalPagesInCurrentSpine) {
  CrossPointPosition out{};
  if (!epub) return out;
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) return out;

  // Spine resolution priority (handoff §7.3):
  //   1. xpointer DocFragment[N]   — exact, our preferred case
  //   2. CFI spine step            — phone-only writers without prior KOReader sync
  //   3. byte ratio from progress  — last-resort approximation
  int spineIdx = -1;
  const int frag = parseIndexAfter(rPos.xpointer, "/body/DocFragment[", false);
  if (frag >= 1 && frag - 1 < spineCount) spineIdx = frag - 1;

  if (spineIdx < 0 && !rPos.location.empty()) {
    const int step = parseCfiSpineStep(rPos.location);
    if (step >= 2) {
      const int candidate = (step - 2) / 2;
      if (candidate >= 0 && candidate < spineCount) spineIdx = candidate;
    }
  }

  const size_t bookSize = epub->getBookSize();
  if (spineIdx < 0 && rPos.progressTotal > 0 && bookSize > 0) {
    const float ratio =
        std::clamp(static_cast<float>(rPos.progressCurrent) / static_cast<float>(rPos.progressTotal), 0.0f, 1.0f);
    const size_t targetBytes = static_cast<size_t>(bookSize * ratio);
    for (int i = 0; i < spineCount; ++i) {
      if (epub->getCumulativeSpineItemSize(i) >= targetBytes) {
        spineIdx = i;
        break;
      }
    }
  }
  if (spineIdx < 0) return out;
  out.spineIndex = spineIdx;

  // Paragraph index: deepest `/p[N]` in the xpointer. Sub-paragraph offsets
  // (`/text().<n>`) are ignored for V1 — CrossPoint navigates at paragraph
  // granularity, so the offset would be discarded anyway.
  const int xpathP = parseIndexAfter(rPos.xpointer, "/p[", true);
  if (xpathP > 0) {
    out.paragraphIndex = static_cast<uint16_t>(xpathP);
    out.hasParagraphIndex = true;
  }

  // Pagination: prefer caller's exact pagination if they're already on the
  // target spine. Otherwise rescale by byte-density relative to the current
  // spine the caller knows about. The reader will ultimately repaginate on
  // navigation, so totalPages is a hint, not a contract.
  if (spineIdx == currentSpineIndex && totalPagesInCurrentSpine > 0) {
    out.totalPages = totalPagesInCurrentSpine;
  } else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
    const size_t curPrev = (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t curSize = epub->getCumulativeSpineItemSize(currentSpineIndex) - curPrev;
    const size_t prev = (spineIdx > 0) ? epub->getCumulativeSpineItemSize(spineIdx - 1) : 0;
    const size_t size = epub->getCumulativeSpineItemSize(spineIdx) - prev;
    if (curSize > 0) {
      out.totalPages = std::max(
          1, static_cast<int>(totalPagesInCurrentSpine * static_cast<float>(size) / static_cast<float>(curSize)));
    }
  }

  // Intra-spine page from the book-wide ratio. paragraphIndex (if present)
  // is the more precise signal — leave it for the reader to consume — but
  // pageNumber still needs a sensible best-guess for the initial render.
  if (out.totalPages > 0 && rPos.progressTotal > 0 && bookSize > 0) {
    const size_t prev = (spineIdx > 0) ? epub->getCumulativeSpineItemSize(spineIdx - 1) : 0;
    const size_t size = epub->getCumulativeSpineItemSize(spineIdx) - prev;
    if (size > 0) {
      const float bookRatio =
          std::clamp(static_cast<float>(rPos.progressCurrent) / static_cast<float>(rPos.progressTotal), 0.0f, 1.0f);
      const size_t targetBytes = static_cast<size_t>(bookSize * bookRatio);
      const size_t bytesIn = (targetBytes > prev) ? (targetBytes - prev) : 0;
      const float intra = std::clamp(static_cast<float>(bytesIn) / static_cast<float>(size), 0.0f, 1.0f);
      out.pageNumber = std::clamp(static_cast<int>(intra * out.totalPages), 0, out.totalPages - 1);
    }
  }

  LOG_DBG("RPM", "<- R: xpointer=%s progress=[%d,%d] -> spine=%d page=%d/%d para=%u%s", rPos.xpointer.c_str(),
          rPos.progressCurrent, rPos.progressTotal, out.spineIndex, out.pageNumber, out.totalPages, out.paragraphIndex,
          out.hasParagraphIndex ? "" : " (no para)");
  return out;
}
