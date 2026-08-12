#include "PageFlipTransportFactory.h"

#include <Memory.h>

#if defined(ARDUINO_ARCH_ESP32)
#include "PageFlipEspNowTransport.h"
#else
#include "PageFlipUdpTransport.h"
#endif

std::unique_ptr<PageFlipTransport> makePageFlipTransport() {
#if defined(ARDUINO_ARCH_ESP32)
  return makeUniqueNoThrow<PageFlipEspNowTransport>();
#else
  return makeUniqueNoThrow<PageFlipUdpTransport>();
#endif
}
