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

PageFlipRole defaultPageFlipRole() {
#if defined(ARDUINO_ARCH_ESP32)
  // TODO(section 9 step 8): read the per-device setting once the pairing UI exists. Both devices
  // reporting Left means no role offset is applied, which shows the same page on both -- obviously
  // wrong for a real pair, and harmless only because the device link is unmeasured anyway.
  return PageFlipRole::Left;
#else
  // The UDP slot already carries a device identity, so two simulator instances take opposite roles
  // from their launch environment with nothing to configure.
  return PageFlipUdpTransport::readConfiguredSlot() == 0 ? PageFlipRole::Left : PageFlipRole::Right;
#endif
}
