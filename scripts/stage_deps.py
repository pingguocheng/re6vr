#!/usr/bin/env python3
"""Download + stage build dependencies into C:\\re6vr\\_third_party."""
import os
import shutil
import sys
import tarfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, "_third_party")
DL = os.path.join(TP, "_dl")
UA = {"User-Agent": "re6vr-dep-fetcher/1.0"}

ITEMS = [
    ("openxr_sdk_source.tar.gz",
     "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.63/OpenXR-SDK-Source-release-1.1.63.tar.gz"),
    ("openxr_loader_windows.zip",
     "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-1.1.63/openxr_loader_windows-1.1.63.zip"),
    ("MinHook_134_bin.zip",
     "https://github.com/TsudaKageyu/minhook/releases/download/v1.3.4/MinHook_134_bin.zip"),
    ("MinHook_134_lib.zip",
     "https://github.com/TsudaKageyu/minhook/releases/download/v1.3.4/MinHook_134_lib.zip"),
]


def download(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        print(f"cached  {os.path.basename(dest)} ({os.path.getsize(dest)} bytes)")
        return
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    print(f"fetch   {url}")
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=300) as r, open(dest, "wb") as f:
        total = 0
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            total += len(chunk)
    print(f"        -> {os.path.basename(dest)} ({total} bytes)")


def main():
    os.makedirs(DL, exist_ok=True)
    for name, url in ITEMS:
        try:
            download(url, os.path.join(DL, name))
        except Exception as e:
            print(f"FAILED {name}: {e}")
            return 1

    # --- stage OpenXR headers + loader ---
    inc = os.path.join(TP, "openxr", "include", "openxr")
    os.makedirs(inc, exist_ok=True)
    with tarfile.open(os.path.join(DL, "openxr_sdk_source.tar.gz")) as tf:
        want = [m for m in tf.getmembers()
                if m.isfile() and "openxr/" in m.name and m.name.endswith((".h", ".hpp"))]
        for m in want:
            data = tf.extractfile(m).read()
            with open(os.path.join(inc, os.path.basename(m.name)), "wb") as f:
                f.write(data)
        print(f"openxr headers staged: {len(want)} files -> {inc}")
        if len(want) < 10:
            print("  !! unexpected header count, sample entries:")
            for m in tf.getmembers()[:40]:
                if m.isfile():
                    print("    ", m.name)

    loader_dir = os.path.join(TP, "openxr", "loader")
    os.makedirs(loader_dir, exist_ok=True)
    # Only the desktop (non-UWP) loader is usable by a Win32 game process.
    if os.path.isdir(loader_dir):
        shutil.rmtree(loader_dir)
    prefix_map = {"Win32/": "x86", "x64/": "x64"}
    with zipfile.ZipFile(os.path.join(DL, "openxr_loader_windows.zip")) as z:
        for n in z.namelist():
            low = n.replace("\\", "/")
            for prefix, arch in prefix_map.items():
                if low.startswith(prefix) and low.endswith((".dll", ".lib")):
                    base = os.path.basename(low)
                    dest = os.path.join(loader_dir, arch, base)
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    with z.open(n) as src, open(dest, "wb") as f:
                        f.write(src.read())
                    print(f"  loader {arch}/{base} ({os.path.getsize(dest)} bytes)")

    # --- stage MinHook ---
    for zname in ("MinHook_134_bin.zip", "MinHook_134_lib.zip"):
        with zipfile.ZipFile(os.path.join(DL, zname)) as z:
            for n in z.namelist():
                low = n.lower().replace("\\", "/")
                if low.endswith("/"):
                    continue
                base = os.path.basename(low)
                if base.lower() not in ("minhook.h", "libminhook.x86.lib", "libminhook.x64.lib",
                                        "minhook.x86.lib", "minhook.x64.lib", "minhook.x86.dll",
                                        "minhook.x64.dll"):
                    continue
                dest_dir = os.path.join(TP, "minhook")
                os.makedirs(dest_dir, exist_ok=True)
                with z.open(n) as src, open(os.path.join(dest_dir, base), "wb") as f:
                    f.write(src.read())
                print(f"  minhook {base} ({os.path.getsize(os.path.join(dest_dir, base))} bytes)")

    print("\n=== staged tree ===")
    for base, dirs, files in os.walk(TP):
        if "_dl" in base:
            continue
        for fn in files:
            p = os.path.join(base, fn)
            print(f"  {os.path.relpath(p, TP)}  {os.path.getsize(p)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
