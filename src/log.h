// log.h - tiny thread-safe logger that writes next to the game executable.
#pragma once

#include <windows.h>

namespace vrlog {

// Opens <dir-of-this-module>\re6vr.log (append) and mirrors every line to
// OutputDebugStringW. Safe to call from any thread; no-op if it fails.
void init(HMODULE self_module);

void write_line(const char *fmt, ...);

void shutdown();

// Returns the path of the log file (empty before init()).
const wchar_t *path();

} // namespace vrlog

#define VRLOG(...) ::vrlog::write_line(__VA_ARGS__)
