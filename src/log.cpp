#include "log.h"

#include <fcntl.h>
#include <io.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace vrlog {
namespace {

CRITICAL_SECTION g_cs;
bool             g_cs_ready = false;
FILE            *g_file     = nullptr;
wchar_t          g_path[MAX_PATH] = L"";

void ensure_cs() {
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
    }
}

// Returns true if `dir` exists and `name` can be read there; `out` gets the
// full path when it does.
bool dir_has_file(const wchar_t *dir, const wchar_t *name, wchar_t *out, size_t out_count) {
    if (!dir || !dir[0]) return false;
    _snwprintf_s(out, out_count, _TRUNCATE, L"%s%s", dir, name);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

void init(HMODULE self_module) {
    ensure_cs();
    EnterCriticalSection(&g_cs);
    if (!g_file) {
        wchar_t module_path[MAX_PATH] = L"";
        wchar_t module_dir[MAX_PATH] = L"";
        if (self_module && GetModuleFileNameW(self_module, module_path, MAX_PATH)) {
            wchar_t *slash = wcsrchr(module_path, L'\\');
            if (slash) {
                *(slash + 1) = L'\0';
                wcscpy_s(module_dir, module_path);
                _snwprintf_s(g_path, MAX_PATH, _TRUNCATE, L"%sre6vr.log", module_path);
            }
        }

        // The log directory is also the directory every marker file is read
        // from, and the game's own folder is generally not writable without
        // elevation - which made every switch expensive to test. So:
        //
        //   RE6VR_LOG_DIR=<dir>          environment (works when launched directly)
        //   <proxy dir>\re6vr_logdir.txt  first line = the directory
        //
        // If a re6vr.log already sits next to the proxy it is used as before, so
        // nothing changes for a deployment that does not ask for this.
        wchar_t dir[MAX_PATH] = L"";
        wchar_t buf[MAX_PATH] = L"";
        if (module_dir[0] &&
            GetEnvironmentVariableW(L"RE6VR_LOG_DIR", buf, MAX_PATH) > 0) {
            _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s", buf);
            const size_t n = wcslen(dir);
            if (n && dir[n - 1] != L'\\' && n + 1 < MAX_PATH) {
                dir[n] = L'\\';
                dir[n + 1] = L'\0';
            }
        } else if (module_dir[0]) {
            wchar_t marker[MAX_PATH] = L"";
            if (dir_has_file(module_dir, L"re6vr_logdir.txt", marker, MAX_PATH)) {
                FILE *f = _wfopen(marker, L"r");
                if (f) {
                    if (fgetws(buf, MAX_PATH, f)) {
                        size_t n = wcslen(buf);
                        while (n && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r' ||
                                     buf[n - 1] == L' ' || buf[n - 1] == L'\t')) {
                            buf[--n] = L'\0';
                        }
                        if (n) {
                            _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s", buf);
                            const size_t m = wcslen(dir);
                            if (dir[m - 1] != L'\\' && m + 1 < MAX_PATH) {
                                dir[m] = L'\\';
                                dir[m + 1] = L'\0';
                            }
                        }
                    }
                    fclose(f);
                }
            }
        }
        if (dir[0] && GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
            _snwprintf_s(g_path, MAX_PATH, _TRUNCATE, L"%sre6vr.log", dir);
        } else if (dir[0] && dir[0] != L'\\' && module_dir[0]) {
            // A relative directory is resolved against the proxy's own folder,
            // so "<proxy dir>\re6vr_work" works and one short line in the marker
            // file is enough. Steam launches the game with its own working
            // directory, so resolving against "." would point somewhere else.
            wchar_t joined[MAX_PATH] = L"";
            _snwprintf_s(joined, MAX_PATH, _TRUNCATE, L"%s%s", module_dir, dir);
            if (GetFileAttributesW(joined) != INVALID_FILE_ATTRIBUTES) {
                _snwprintf_s(g_path, MAX_PATH, _TRUNCATE, L"%sre6vr.log", joined);
            }
        }
        if (g_path[0] == L'\0') {
            wcscpy_s(g_path, L"re6vr.log");
        }
        // UTF-8 with BOM so the file reads correctly in any editor. Opened through
        // CreateFileW with FILE_SHARE_READ on purpose: MSVC's fopen hands out a
        // handle nothing else can read, and the log is the main diagnostic for a
        // running game - being unable to tail it while the game is up is what made
        // the first fullscreen run hard to read.
        HANDLE h = CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_BINARY);
            if (fd != -1) {
                g_file = _fdopen(fd, "wb");
                if (!g_file) _close(fd);
            } else {
                CloseHandle(h);
            }
        }
        if (g_file) {
            fwrite("\xEF\xBB\xBF", 1, 3, g_file);
            fflush(g_file);
        }
    }
    LeaveCriticalSection(&g_cs);

    SYSTEMTIME st;
    GetLocalTime(&st);
    write_line("=== re6vr proxy attached %04d-%02d-%02d %02d:%02d:%02d pid=%lu ===",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               (unsigned long)GetCurrentProcessId());
}

void write_line(const char *fmt, ...) {
    ensure_cs();

    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2304];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d t=%lu] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentThreadId(), body);

    EnterCriticalSection(&g_cs);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    LeaveCriticalSection(&g_cs);

    // Mirror to the debugger (DbgView / VS output window).
    wchar_t wide[2304];
    MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, 2304);
    OutputDebugStringW(wide);
}

void shutdown() {
    ensure_cs();
    EnterCriticalSection(&g_cs);
    if (g_file) {
        fputs("=== re6vr proxy detaching ===\n", g_file);
        fclose(g_file);
        g_file = nullptr;
    }
    LeaveCriticalSection(&g_cs);
}

const wchar_t *path() { return g_path; }

} // namespace vrlog
