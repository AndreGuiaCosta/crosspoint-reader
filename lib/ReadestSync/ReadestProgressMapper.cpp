#include "ReadestProgressMapper.h"

#include <Logging.h>

#include <algorithm>
#include <cmath>

#include "ProgressMapper.h"

namespace {
// Extract the second `/N` step from an `epubcfi(/A/B!/...)` string.
// The leading step is `/6` for the spine collection; 0-based spine =
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

  // Reuse the KOSync-side mapper for xpointer resolution: it owns the
  // intra → xpath fallback chain (progress-based, then paragraph-based, then
  // synthesized DocFragment) and the upstream off-by-one fix for
  // pageNumber ↔ intra. Readest accepts the same KOReader-style xpath.
  const KOReaderPosition koPos = ProgressMapper::toKOReader(epub, pos);
  r.xpointer = koPos.xpath;
  if (r.xpointer.empty()) {
    // Section-granularity fallback: bare DocFragment is enough to jump to
    // the correct chapter.
    r.xpointer = "/body/DocFragment[" + std::to_string(pos.spineIndex + 1) + "]";
  }

  // CFI deliberately left empty — receiver falls back to xpointer.
  r.location.clear();

  // Estimate book-wide pages from current-spine byte density. Section-local
  // numbers would jump at every chapter boundary on the receiving client.
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
  if (!epub) return {};

  // Translate Readest position into a KOReader-format position and delegate.
  // ProgressMapper handles spine resolution (DocFragment[N] in xpath, then
  // byte-ratio from percentage), paragraphIndex/liIndex/anchor extraction,
  // totalPages density rescale, and the upstream-fixed intra → pageNumber
  // formula. The only Readest-specific input is CFI: when xpointer doesn't
  // pin the spine, parse the CFI's spine step and synthesize a DocFragment
  // xpath so ProgressMapper can take it from there.
  KOReaderPosition koPos;
  koPos.xpath = rPos.xpointer;
  if (koPos.xpath.find("/body/DocFragment[") == std::string::npos && !rPos.location.empty()) {
    const int step = parseCfiSpineStep(rPos.location);
    if (step >= 2) {
      const int spineCount = epub->getSpineItemsCount();
      const int candidate = (step - 2) / 2;
      if (candidate >= 0 && candidate < spineCount) {
        koPos.xpath = "/body/DocFragment[" + std::to_string(candidate + 1) + "]";
      }
    }
  }
  koPos.percentage =
      (rPos.progressTotal > 0)
          ? std::clamp(static_cast<float>(rPos.progressCurrent) / static_cast<float>(rPos.progressTotal), 0.0f, 1.0f)
          : 0.0f;

  CrossPointPosition out = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, totalPagesInCurrentSpine);

  LOG_DBG("RPM", "<- R: xpointer=%s progress=[%d,%d] -> spine=%d page=%d/%d para=%u%s", rPos.xpointer.c_str(),
          rPos.progressCurrent, rPos.progressTotal, out.spineIndex, out.pageNumber, out.totalPages, out.paragraphIndex,
          out.hasParagraphIndex ? "" : " (no para)");
  return out;
}
