// crash_probe_main.cpp - a tiny loader whose only job is to record *why* the
// proxy DLL faults on load.
//
// The proxy dies between the first line of DllMain and the first trace write,
// which no in-DLL logging can observe. This host installs a vectored exception
// handler before the load and prints the exception code, the faulting address
// and the owning module, so the crash can be classified (null write, stack
// overflow, bad jump) instead of guessed at.
//
// Build: build_crashprobe.bat -> build\_probe\crashprobe.exe
// Run:   from a directory containing the d3d9.dll under test.
#include <windows.h>

#include <cstdio>

static void append_line(const char *file, const char *text) {
    HANDLE f = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, text, (DWORD)lstrlenA(text), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

static LONG WINAPI veh(EXCEPTION_POINTERS *info) {
    char buf[512];
    const EXCEPTION_RECORD *er = info->ExceptionRecord;
    void *addr = er->ExceptionAddress;
    HMODULE mod = nullptr;
    char modname[MAX_PATH] = "(unknown)";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod)) {
        GetModuleFileNameA(mod, modname, MAX_PATH);
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "VEH: code=0x%08lX addr=%p module=%s",
                (unsigned long)er->ExceptionCode, addr, modname);
    append_line("re6vr_crash.txt", buf);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "VEH: %s address=%p",
                    er->ExceptionInformation[0] == 1 ? "write to" : "read from",
                    (void *)er->ExceptionInformation[1]);
        append_line("re6vr_crash.txt", buf);
    }
    if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        append_line("re6vr_crash.txt", "VEH: stack overflow");
    }
    return EXCEPTION_CONTINUE_SEARCH; // let the process die as it normally would
}

int main(int argc, char **argv) {
    char dll[MAX_PATH] = "d3d9.dll";
    if (argc > 1) {
        lstrcpynA(dll, argv[1], MAX_PATH);
    }
    if (argc > 2) {
        SetCurrentDirectoryA(argv[2]);
        char cwd[MAX_PATH] = "";
        GetCurrentDirectoryA(MAX_PATH, cwd);
        char msg[MAX_PATH + 32];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "host: cwd=%s", cwd);
        append_line("re6vr_crash.txt", msg);
    }

    DeleteFileA("re6vr_crash.txt");
    AddVectoredExceptionHandler(1, &veh);

    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "host: loading %s", dll);
    append_line("re6vr_crash.txt", buf);
    printf("%s\n", buf);

    HMODULE mod = LoadLibraryA(dll);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "host: LoadLibraryA -> %p (err %lu)",
                (void *)mod, mod ? 0UL : GetLastError());
    append_line("re6vr_crash.txt", buf);
    printf("%s\n", buf);

    if (mod) {
        FARPROC p = GetProcAddress(mod, "Direct3DCreate9");
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "host: Direct3DCreate9 -> %p", (void *)p);
        append_line("re6vr_crash.txt", buf);
        printf("%s\n", buf);
    }
    return mod ? 0 : 1;
}
