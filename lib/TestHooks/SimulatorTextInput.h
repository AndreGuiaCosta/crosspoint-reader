#pragma once
#include <string>

/**
 * Simulator-only test hook for KeyboardEntryActivity.
 *
 * Production firmware ships a no-op `.cpp` (in `src/test_hooks/`) so this
 * function returns an empty string and the calling `if`-branch is dead-coded
 * by the optimiser — zero overhead, no on-device behaviour change.
 *
 * The simulator's `build_src_filter` excludes the firmware no-op and the
 * `crosspoint-simulator` library provides its own implementation that
 * forwards to `ScriptDriver::consumeQueuedText()`. This is how a `type ...`
 * script command bypasses the on-screen keyboard navigation.
 *
 * The header is identical in both builds; only the linked `.cpp` differs.
 */
namespace SimulatorTextInput {
std::string consumeQueuedText();
}  // namespace SimulatorTextInput
