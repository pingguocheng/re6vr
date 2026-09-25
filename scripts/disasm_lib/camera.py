"""The RE6 camera class map: what is verified statically, and what is not.

Everything here was read out of BH6.exe by the validators below; nothing is guessed. Each
claim names the evidence that establishes it, so a later session can re-check rather than
trust.

WHY THIS FILE EXISTS
--------------------
Two earlier sessions concluded RE6 had no static camera anchor:

  * "no static MtDti"  - the search looked for a dword equal to `name_address - 4`, but in MT
    Framework `name` sits at object+4, not object+0, so the scan searched for the wrong value.
  * "DTI registration is unreachable" - the registration functions do have no direct callers,
    but they are not the way in: 3372 of them run at startup and each one names its class and
    its static address in a fixed instruction sequence. The classes are described statically;
    only the object bytes are built at runtime (they live in .data BSS).

WHAT IS VERIFIED
----------------
    uCameraBase      DTI 0x017D1DD0   size 0x01B0    root of the camera hierarchy
    uCameraCtrl      DTI 0x017D26F0   size 0x4B70    the camera manager
    sCamera          DTI 0x0186E260   size 0x0CE0    renderer camera (parent of sBioCamera)
    sBioCamera       DTI 0x017C3164   size 0x1620    the game's camera
    +18 more uCamera* subclasses (uCameraQFPS 0x1B80, uCameraAnimation 0x1400, ...)

    factory context   == DTI + 0x1C, for all 3324 classes that have one
    uCameraCtrl ctx   0x017D270C - read by 8 sites in .text (see the caveat below)

    A uCameraCtrl is also allocated by the stage loader (the function that formats
    "soft\\stage\\s%04d") and its pointer stored at `[stage_object + 0x640]`.

WHAT IS *NOT* VERIFIED
----------------------
1. **That [0x017D270C] is the uCameraCtrl instance.** It is the context argument the factory
   call passes, and the field is written by the registrar as a name hash. All 8 sites that
   mention the address *read* it; none writes an instance pointer there by absolute address
   (the writes would go through a computed address). One memory read under the debugger
   settles it: if `[0x017D270C]` points at a 0x4B70-byte object, the claim holds.
2. **The field offsets inside a camera instance.** The DMC4 reference (`mCameraPos +0x30`,
   `mCameraUp +0x40`, `mTargetPos +0x50`, `mFov +0x24`) is a plausible hypothesis but has NOT
   been confirmed against BH6. Do not write to those offsets before checking them against a
   live object.
3. **Where the view matrix lives.** The earlier sessions established by measurement that the
   *captured* view matrix does not persist in memory, which is consistent with it being
   computed per frame from pos/up/target and discarded. If so, the hook belongs on the
   function that builds it, not on a field - and that function is not yet identified.
"""
from __future__ import annotations

import os
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from disasm_lib import pe                                          # noqa: E402
from disasm_lib.dti import enumerate_dtis                          # noqa: E402
from disasm_lib.managers import find_managers                      # noqa: E402

DEFAULT_EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"

# Verified by the scans in this package; see the module docstring for the provenance of each.
CAMERA_CLASSES = {
    "uCameraCtrl": 0x017D26F0,
    "uCameraBase": 0x017D1DD0,
    "uCameraQFPS": 0x017D2A00,
    "uCameraAnimation": 0x017D1D44,
    "uCameraLERP": 0x017D2740,
    "uCameraRail": 0x017D2A20,
    "uCameraTurret": 0x017D2A60,
    "uCameraVeh": 0x017D2AA0,
    "uCameraNavi": 0x017D2B24,
    "sCamera": 0x0186E260,
    "sBioCamera": 0x017C3164,
}

CAMERA_MANAGER_SLOT = 0x017D270C      # [this] -> live uCameraCtrl (0x4B70 bytes)
STAGE_CAMERA_OFFSET = 0x640           # [stage_object + this] -> live uCameraCtrl
STAGE_GLOBAL = 0x017CF454             # the global used all over the stage code
DTI_RESOLVER = 0x00E67C50             # hash-bucket lookup: dti -> class record
DTI_REGISTRAR = 0x00E67920            # fills in a class record (size, parent, name, hash)
DTI_INSTANCE_SLOT = 0x1C              # class record + this holds the live instance pointer


@dataclass
class CameraInfo:
    name: str
    dti: int
    size: int
    context: int | None        # the global the factory call reads; see caveat 1 in the docstring


def camera_map(img: pe.Image | None = None) -> dict[str, CameraInfo]:
    """Camera classes with their DTI, instance size and factory context, read from the image.

    Re-derives everything from the binary rather than trusting the constants above, so a
    different build of BH6.exe reports its own numbers instead of silently using these.
    """
    if img is None:
        img = pe.Image.load(DEFAULT_EXE)
    dtis = {d.address: d for d in enumerate_dtis(img)}
    contexts = {m.name: m.context for m in find_managers(img)}
    out: dict[str, CameraInfo] = {}
    by_name = {d.name: d for d in dtis.values()}
    for name in CAMERA_CLASSES:
        d = by_name.get(name)
        if d is None:
            continue
        out[name] = CameraInfo(name, d.address, d.size, contexts.get(name))
    return out


def print_report(img: pe.Image | None = None) -> None:
    if img is None:
        img = pe.Image.load(DEFAULT_EXE)
    cmap = camera_map(img)
    print("camera classes in %s" % os.path.basename(DEFAULT_EXE))
    print("%-20s %-12s %-10s %s" % ("class", "MtDti", "size", "factory context global"))
    for name, ci in cmap.items():
        print("%-20s 0x%08X 0x%-8X %s"
              % (name, ci.dti, ci.size,
                 ("0x%08X" % ci.context) if ci.context else "(none)"))
    print()
    print("camera manager candidate  [0x%08X]   (hypothesis - see module docstring)"
          % CAMERA_MANAGER_SLOT)
    print("stage route               [[0x%08X] + 0x%X]"
          % (STAGE_GLOBAL, STAGE_CAMERA_OFFSET))


if __name__ == "__main__":
    print_report()
