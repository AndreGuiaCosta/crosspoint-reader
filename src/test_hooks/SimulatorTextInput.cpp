#include <SimulatorTextInput.h>

// Production no-op. Excluded from the simulator build by `build_src_filter`
// in `platformio.local.ini`; the `crosspoint-simulator` library provides
// the real implementation in its own translation unit.
namespace SimulatorTextInput {
std::string consumeQueuedText() { return {}; }
}  // namespace SimulatorTextInput
