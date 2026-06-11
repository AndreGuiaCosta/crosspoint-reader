#pragma once

#include <string>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)

// Readest: low-heap sign-in recovery. Heap-tight devices can't fit the TLS
// handshake next to WiFi from the settings flow; park the typed password in
// RTC memory (survives ESP.restart(), wiped on power loss, cleared on
// consume) and rerun the sign-in early in setup(), before the heavy
// singletons load.
void setSilentRebootAuthPassword(const std::string& pw);
void silentRestartToReadestAuth();
