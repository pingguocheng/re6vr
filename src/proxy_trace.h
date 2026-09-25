// proxy_trace.h - crash forensics for the proxy DLL.
//
// A fault inside DllMain kills the process before any buffered logging can be
// flushed, which makes "no log file at all" ambiguous. PT_STEP() appends a
// marker to a tiny side file next to the game executable, flushes it and closes
// it again, so the last marker on disk tells us exactly where execution stopped.
#pragma once

#include <windows.h>

#include <cstdio>

namespace ptrace {

inline void step(const char *text) {
    wchar_t dir[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *(slash + 1) = L'\0';

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sre6vr_trace.txt", dir);

    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[256];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s\n", text);
    DWORD written = 0;
    if (n > 0) WriteFile(f, line, (DWORD)n, &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

} // namespace ptrace

#define PT_STEP(text) ::ptrace::step(text)

// ---------------------------------------------------------------- crash address reporter
//
// PT_STEP answers "how far did it get"; it cannot answer "what faulted". A first-chance vectored
// exception handler can, and it is cheap enough to leave installed: it appends ONE line with the
// exception code, the faulting instruction and the address it touched, then lets the exception
// continue to the default handler so nothing about the crash changes.
//
// This exists because the offline harness crashed inside the camera probe's vtable scan
// (0xC0000005) with the log ending between two lines, and "which access, at which instruction" is
// the difference between a five-minute fix and an evening of guessing.
namespace ptrace {

inline LONG WINAPI crash_filter(EXCEPTION_POINTERS *info) {
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    wchar_t dir[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sre6vr_crash.txt", dir);

    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;

    DWORD bad = 0;
    const char *kind = "?";
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR rw = info->ExceptionRecord->ExceptionInformation[0];
        bad = (DWORD)info->ExceptionRecord->ExceptionInformation[1];
        kind = rw == 0 ? "read" : (rw == 1 ? "write" : "execute");
    }
    char line[320];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "FAULT code=%08lX (%s at %08X) instruction=%p thread=%lu\n",
                        (unsigned long)code, kind, bad,
                        (void *)info->ExceptionRecord->ExceptionAddress,
                        (unsigned long)GetCurrentThreadId());
    DWORD written = 0;
    if (n > 0) WriteFile(f, line, (DWORD)n, &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

inline void install_crash_reporter() {
    static bool done = false;
    if (done) return;
    done = true;
    AddVectoredExceptionHandler(1 /* first */, &crash_filter);
}

} // namespace ptrace

#define PT_INSTALL_CRASH_REPORTER() ::ptrace::install_crash_reporter()
