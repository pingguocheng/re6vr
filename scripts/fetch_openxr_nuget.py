#!/usr/bin/env python3
"""Fetch the generated OpenXR headers + prebuilt loader from NuGet.

include/openxr/*.h is generated at build time and is absent from the
OpenXR-SDK-Source tarball, and raw.githubusercontent 404s for it, so NuGet is
the reliable source. The `OpenXR` package ships both the spec-matching headers
and the loader.
"""
import json
import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, "_third_party")
INC = os.path.join(TP, "openxr", "include", "openxr")
LOADER = os.path.join(TP, "openxr", "loader")
DL = os.path.join(TP, "_dl")
UA = {"User-Agent": "re6vr-dep-fetcher/1.0"}
FLAT = "https://api.nuget.org/v3-flatcontainer"
PKG = "OpenXR"

REQUIRED_SYMBOLS = [
    "xrCreateInstance", "xrGetSystem", "xrCreateSession", "xrCreateSwapchain",
    "xrAcquireSwapchainImage", "xrWaitSwapchainImage", "xrReleaseSwapchainImage",
    "xrLocateViews", "xrEndFrame", "xrPollEvent", "xrBeginSession",
]


def fetch(url, dest):
    if not os.path.exists(dest) or os.path.getsize(dest) == 0:
        print(f"fetch  {url}")
        with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=180) as r, open(dest, "wb") as f:
            f.write(r.read())
    print(f"cached {os.path.basename(dest)} ({os.path.getsize(dest)} bytes)")
    return dest


def main():
    os.makedirs(INC, exist_ok=True)
    os.makedirs(DL, exist_ok=True)
    if os.path.isdir(LOADER):
        import shutil
        shutil.rmtree(LOADER)
    os.makedirs(LOADER, exist_ok=True)

    idx = json.loads(urllib.request.urlopen(
        urllib.request.Request(f"{FLAT}/{PKG.lower()}/index.json", headers=UA), timeout=60).read())
    versions = [v for v in idx["versions"] if "-" not in v]
    version = versions[-1]
    print(f"package {PKG} -> version {version}")

    nupkg = fetch(f"{FLAT}/{PKG.lower()}/{version}/{PKG.lower()}.{version}.nupkg",
                  os.path.join(DL, f"{PKG}.{version}.nupkg"))

    headers = {}
    loaders = []
    with zipfile.ZipFile(nupkg) as z:
        for n in z.namelist():
            low = n.replace("\\", "/")
            base = os.path.basename(low)
            if low.lower().endswith(".h") and "openxr" in low.lower():
                data = z.read(n)
                headers[base] = data
            elif low.lower().endswith((".dll", ".lib")):
                if "win32" in low or "x86" in low:
                    arch = "x86"
                elif "x64" in low or "amd64" in low:
                    arch = "x64"
                else:
                    continue
                loaders.append((arch, base, z.read(n)))

    for base, data in headers.items():
        with open(os.path.join(INC, base), "wb") as f:
            f.write(data)
        print(f"  header {base} ({len(data)} bytes)")

    # Loader packages often carry a static and a dynamic build of the same name;
    # keep the largest (the full static loader DLL).
    best = {}
    for arch, base, data in loaders:
        key = (arch, base.lower())
        if key not in best or len(data) > len(best[key][2]):
            best[key] = (arch, base, data)
    for (arch, _), (_, base, data) in sorted(best.items()):
        dest = os.path.join(LOADER, arch, base)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
        print(f"  loader {arch}/{base} ({len(data)} bytes)")

    ok = True
    xh = os.path.join(INC, "openxr.h")
    if not os.path.exists(xh):
        print("MISSING openxr.h")
        ok = False
    else:
        text = open(xh, encoding="utf-8", errors="replace").read()
        missing = [s for s in REQUIRED_SYMBOLS if s not in text]
        if missing:
            print("openxr.h missing symbols:", missing)
            ok = False
        else:
            print("openxr.h contains every symbol we compile against: OK")
    plat = open(os.path.join(INC, "openxr_platform.h"), encoding="utf-8", errors="replace").read() \
        if os.path.exists(os.path.join(INC, "openxr_platform.h")) else ""
    if "XrGraphicsBindingD3D11KHR" not in plat:
        print("openxr_platform.h missing XrGraphicsBindingD3D11KHR")
        ok = False
    if not os.path.exists(os.path.join(LOADER, "x86", "openxr_loader.dll")):
        print("MISSING x86 loader dll")
        ok = False
    print("RESULT:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
