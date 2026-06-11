#pragma once

#include <cstdint>
#include <string>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();              // home screen
void silentRestartToReader();      // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToFileTransfer();// goes straight back to File Transfer activity

// CrumBLE: optional mode hint persisted across silentRestartToFileTransfer
// so the FT activity skips the mode-picker on auto-recover.
// Valid values: 0 = no hint, 1 = JOIN_NETWORK, 2 = CREATE_HOTSPOT.
void setSilentRebootFtModeHint(uint32_t mode);
uint32_t consumeSilentRebootFtModeHint();

// CrumBLE+Readest: low-heap sign-in recovery. Heap-tight devices can't fit
// the TLS handshake next to WiFi from the settings flow; park the typed
// password in RTC memory (survives ESP.restart(), wiped on power loss,
// cleared on consume) and rerun the sign-in early in setup(), before the
// heavy singletons load.
void setSilentRebootAuthPassword(const std::string& pw);
void silentRestartToReadestAuth();
