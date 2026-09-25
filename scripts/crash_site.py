#!/usr/bin/env python3
"""Run the harness with the camera self-test, then say WHICH MODULE owns the fault address.

The crash reporter in proxy_trace.h gives the faulting instruction address (e.g. 0x7B0EA26F - a
system DLL, not the proxy). This script turns that address into a module name by asking Windows
which module in a live harness process covers it, via a one-line ctypes helper compiled on the fly.
No debugger, no symbols: the fault was a read of address 0, so knowing whose code did it is enough
to find the call site.

Usage: python scripts\\crash_site.py [seconds]
"""
from __future__ import annotations

import ctypes
import os
import subprocess
import sys
from ctypes import wintypes

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
HARNESS = os.path.join(BUILD, "harness.exe")
PROXY = os.path.join(BUILD, "d3d9.dll")
CRASH = os.path.join(BUILD, "re6vr_crash.txt")
TRACE = os.path.join(BUILD, "re6vr_trace.txt")
CREATE_NO_WINDOW = 0x08000000

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
LIST_MODULES_ALL = 0x03


def fault_address() -> int | None:
    if not os.path.exists(CRASH):
        return None
    with open(CRASH, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    for token in text.replace("=", " ").replace("(", " ").split():
        if token.startswith("instruction"):
            continue
    # the crash line is "... instruction=<hex>"
    for part in text.split():
        if part.startswith("instruction="):
            return int(part.split("=", 1)[1], 16)
    return None


class MODULEINFO(ctypes.Structure):
    _fields_ = [("lpBaseOfDll", ctypes.c_void_p),
                ("SizeOfImage", wintypes.DWORD),
                ("EntryPoint", ctypes.c_void_p)]


def module_for(pid: int, addr: int) -> str:
    """Name of the module in `pid` that contains `addr`."""
    h = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        return "(cannot open process %d)" % pid
    try:
        needed = wintypes.DWORD(0)
        psapi.EnumProcessModulesEx(ctypes.c_void_p(h), None, 0, ctypes.byref(needed), LIST_MODULES_ALL)
        count = needed.value // ctypes.sizeof(ctypes.c_void_p)
        if count == 0:
            return "(no modules)"
        arr = (ctypes.c_void_p * count)()
        if not psapi.EnumProcessModulesEx(ctypes.c_void_p(h), arr, needed.value,
                                          ctypes.byref(needed), LIST_MODULES_ALL):
            return "(EnumProcessModulesEx failed %d)" % ctypes.get_last_error()
        for i in range(count):
            info = MODULEINFO()
            if not psapi.GetModuleInformation(ctypes.c_void_p(h), arr[i], ctypes.byref(info),
                                              ctypes.sizeof(info)):
                continue
            base = info.lpBaseOfDll or 0
            if base <= addr < base + info.SizeOfImage:
                buf = ctypes.create_unicode_buffer(260)
                psapi.GetModuleBaseNameW(ctypes.c_void_p(h), arr[i], buf, 260)
                return "%s + 0x%X (base %08X size %X)" % (buf.value, addr - base, base,
                                                          info.SizeOfImage)
        return "(address not inside any module)"
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(h))


def main() -> int:
    seconds = sys.argv[1] if len(sys.argv) > 1 else "4"
    for path in (CRASH, TRACE):
        if os.path.exists(path):
            os.remove(path)
    env = dict(os.environ)
    env["RE6VR_CAM_SELFTEST"] = "1"
    env.pop("RE6VR_SHOT_SELFTEST", None)
    env.pop("RE6VR_SELFTEST", None)
    with open(os.path.join(BUILD, "re6vr_logdir.txt"), "w", encoding="utf-8") as fh:
        fh.write(os.path.join(ROOT, "_work", "harness_log"))

    proc = subprocess.Popen([HARNESS, PROXY, seconds, "320", "240"], cwd=BUILD, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=CREATE_NO_WINDOW)
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
    print("[crash-site] harness rc=%s" % proc.returncode)

    addr = fault_address()
    print("[crash-site] crash file: %s" % (open(CRASH).read().strip() if os.path.exists(CRASH)
                                           else "(none)"))
    if addr:
        print("[crash-site] faulting instruction %08X is in: %s" % (addr, module_for(proc.pid, addr)))
    if os.path.exists(TRACE):
        lines = [l for l in open(TRACE, encoding="utf-8", errors="replace").read().splitlines()
                 if l.strip()]
        print("[crash-site] last trace marker: %s" % (lines[-1] if lines else "(none)"))
    # the log tells which objects the scan was reporting when it died
    log = os.path.join(ROOT, "_work", "harness_log", "re6vr.log")
    if os.path.exists(log):
        tail = open(log, encoding="utf-8", errors="replace").read().splitlines()[-3:]
        for line in tail:
            print("[crash-site] log: %s" % line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
