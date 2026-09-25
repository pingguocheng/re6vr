#!/usr/bin/env python3
"""Fetch the three OpenXR headers we compile against.

The OpenXR-SDK-Source tarball only ships openxr_platform_defines.h (the other
two are generated), so pull them straight from the tagged source tree.
"""
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = os.path.join(ROOT, "_third_party", "openxr", "include", "openxr")
# The release tag does not carry the generated headers in include/openxr, so we
# read them from the branch. They are plain interface definitions; the loader
# we ship is the tagged 1.1.63 build.
REF = "main"
BASE = f"https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK-Source/{REF}/include/openxr/"
FILES = ["openxr.h", "openxr_platform.h", "openxr_platform_defines.h", "openxr_reflection.h"]
UA = {"User-Agent": "re6vr-dep-fetcher/1.0"}


def main():
    os.makedirs(INC, exist_ok=True)
    rc = 0
    for name in FILES:
        dest = os.path.join(INC, name)
        try:
            req = urllib.request.Request(BASE + name, headers=UA)
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
            with open(dest, "wb") as f:
                f.write(data)
            print(f"ok   {name} ({len(data)} bytes)")
        except Exception as e:
            print(f"FAIL {name}: {e}")
            rc = 1
    # sanity: openxr.h must define the version header macro
    p = os.path.join(INC, "openxr.h")
    if os.path.exists(p):
        head = open(p, "rb").read(4000).decode("utf-8", "replace")
        if "XR_CURRENT_API_VERSION" not in head:
            print("WARN openxr.h looks wrong (no XR_CURRENT_API_VERSION in header)")
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
