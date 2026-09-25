// loadtest.c - does the proxy's DllMain survive LoadLibrary?
//
// The harness crashes with an access violation before printing anything, which
// makes "the DLL fails in DllMain" and "the harness dies later" look identical
// from the outside. This loads the DLL and reports immediately, with stdout
// flushed, so the two are distinguishable.
//
// Build: cl /nologo /O2 loadtest.c
#include <stdio.h>
#include <windows.h>

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "d3d9.dll";
    printf("loadtest: about to LoadLibraryA(\"%s\")\n", path);
    fflush(stdout);
    HMODULE m = LoadLibraryA(path);
    printf("loadtest: LoadLibraryA -> %p (err %lu)\n", (void *)m, GetLastError());
    fflush(stdout);
    if (!m) return 1;
    FARPROC p = GetProcAddress(m, "Direct3DCreate9");
    printf("loadtest: Direct3DCreate9 -> %p\n", (void *)p);
    fflush(stdout);
    printf("loadtest: done\n");
    fflush(stdout);
    return 0;
}
