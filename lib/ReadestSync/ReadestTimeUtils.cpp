#include "ReadestTimeUtils.h"

#include <cctype>
#include <cstdio>

int64_t ReadestTimeUtils::parseIso8601ToMs(const std::string& iso) {
  int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return 0;
  int ms = 0;
  const size_t dot = iso.find('.', 19);
  if (dot != std::string::npos) {
    int frac = 0, digits = 0;
    for (size_t i = dot + 1; i < iso.size() && std::isdigit(static_cast<unsigned char>(iso[i])) && digits < 3;
         ++i, ++digits) {
      frac = frac * 10 + (iso[i] - '0');
    }
    while (digits < 3) {
      frac *= 10;
      ++digits;
    }
    ms = frac;
  }
  Y -= M <= 2;
  const int era = (Y >= 0 ? Y : Y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(Y - era * 400);
  const unsigned doy = (153u * (M > 2 ? M - 3 : M + 9) + 2u) / 5u + D - 1;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
  return days * 86400000LL + static_cast<int64_t>(h) * 3600000LL + static_cast<int64_t>(m) * 60000LL +
         static_cast<int64_t>(s) * 1000LL + ms;
}
