// Simulator-only stubs for the few CrossPointWebServer free functions that the
// (sim-compiled) CrossPointWebServerActivity calls. The real definitions live in
// network/CrossPointWebServer.cpp, which is excluded from the native sim build
// (it depends on the ESP HTTP/WebSocket stack). On device this file is empty.
#ifdef SIMULATOR

bool consumeFtRestartRequest() { return false; }
bool peekFtRestartRequest() { return false; }

#endif  // SIMULATOR
