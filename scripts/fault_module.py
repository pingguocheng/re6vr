#!/usr/bin/env python3
"""Name the module that owns the faulting instruction, without a debugger.

The proxy's crash reporter (src/proxy_trace.h) writes the faulting instruction address to
build\\re6vr_crash.txt when the process takes an access violation. That address alone does not say
whose code it is - but a live harness process can be asked, and the crash happens a second into a
run, so there is time to look while it is still alive.

Usage: python scripts\\fault_module.py [address-hex]
"""
from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
HARNESS = os.path.join(BUILD, "harness.exe")
PROXY = os.path.join(BUILD, "d3d9.dll")
CRASH = os.path.join(BUILD, "re6vr_crash.txt")
CREATE_NO_WINDOW = 0x08000000

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                                       ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR,
                                     wintypes.DWORD]


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p), ("SizeOfImage", wintypes.DWORD),
                ("EntryPoint", ctypes.c_void_p)]


psapi.GetModuleInformation.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                       ctypes.POINTER(MODULEINFO), wintypes.DWORD]


def main() -> int:
    target = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0
    if not target and os.path.exists(CRASH):
        with open(CRASH, encoding="utf-8", errors="replace") as fh:
            for part in fh.read().split():
                if part.startswith("instruction="):
                    target = int(part.split("=", 1)[1], 16)
    print("[fault] looking for %08X" % target)

    env = dict(os.environ)
    env["RE6VR_CAM_SELFTEST"] = "1"
    proc = subprocess.Popen([HARNESS, PROXY, "8", "320", "240"], cwd=BUILD, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=CREATE_NO_WINDOW)
    h = None
    deadline = time.time() + 6
    while time.time() < deadline:
        h = k32.OpenProcess(0x0410, False, proc.pid)
        if h:
            break
        time.sleep(0.1)
    if not h:
        print("[fault] could not open pid %d" % proc.pid)
        proc.kill()
        return 1

    try:
        need = wintypes.DWORD(0)
        psapi.EnumProcessModulesEx(h, None, 0, ctypes.byref(need), 3)
        count = min(need.value // ctypes.sizeof(ctypes.c_void_p), 512)
        arr = (ctypes.c_void_p * max(count, 1))()
        if not psapi.EnumProcessModulesEx(h, arr, count * ctypes.sizeof(ctypes.c_void_p),
                                          ctypes.byref(need), 3):
            print("[fault] EnumProcessModulesEx failed %d" % ctypes.get_last_error())
        mods = []
        for i in range(count):
            mi = MODULEINFO()
            if not psapi.GetModuleInformation(h, arr[i], ctypes.byref(mi), ctypes.sizeof(mi)):
                continue
            buf = ctypes.create_unicode_buffer(260)
            psapi.GetModuleBaseNameW(h, arr[i], buf, 260)
            mods.append((mi.lpBaseOfDll or 0, mi.SizeOfImage, buf.value))
        print("[fault] %d modules" % len(mods))
        hit = False
        for base, size, name in mods:
            if base <= target < base + size:
                print("[fault] %08X is %s + 0x%X (base %08X, size %X)"
                      % (target, name, target - base, base, size))
                hit = True
        if not hit:
            print("[fault] not inside any module - the address is in unowned memory")
            print("[fault] modules above 0x60000000: %s"
                  % ", ".join("%s@%08X" % (n, b) for b, s, n in mods if b > 0x60000000))
    finally:
        k32.CloseHandle(h)
        try:
            proc.kill()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
