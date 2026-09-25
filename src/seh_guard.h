// seh_guard.h - run a D3D9 call that may fault, without taking the game down.
//
// D3D9 is an in-process user-mode API with a long history of faulting on edge
// cases (large readbacks, lost devices, driver quirks). A mod that hooks a game's
// render loop must never let such a fault propagate into the game's own
// exception handling: that turns a dropped frame into a crash. MSVC's /EHa-free
// SEH (__try/__except) lets us catch it and degrade gracefully instead.
#pragma once

#include <windows.h>

namespace seh {

typedef void (*VoidFn)(void *);

// Executes `fn(ctx)` under a structured-exception guard.
// Returns true when the call completed without raising.
inline bool guard(VoidFn fn, void *ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace seh
