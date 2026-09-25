#!/usr/bin/env python3
"""Arm the re6vr markers, point the proxy at them, and deploy - one command, no missed steps.

Why this exists: a run can be wasted by something as small as a marker written to the wrong
directory, or `build\\re6vr_logdir.txt` left pointing at the offline harness's log folder (that has
happened). The proxy reads its log directory from that pointer and reads every switch from there, so
both must be set together, and the deployed DLL must be the one just built.

Usage:
    python scripts\\deploy_config.py                 # deploy the current build with the head config
    python scripts\\deploy_config.py --config observe
    python scripts\\deploy_config.py --gain 0.6 --tripod 0
    python scripts\\deploy_config.py --print         # show the config without touching anything
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
WORK = os.path.join(ROOT, "_work")
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6"

# config name -> the marker files it needs (name -> value). Absolute, because the proxy resolves a
# relative logdir against its own folder and that has bitten this project before.
CONFIGS = {
    "head": {
        "re6vr_steer.txt": "head",
        "re6vr_head_method.txt": "1",        # write inside MakeViewMatrix (stateless)
        "re6vr_head_absolute.txt": "1",      # absolute mapping: the head's angle IS the offset
        "re6vr_head_tripod.txt": "1",        # a left/right turn cannot tilt the view
        "re6vr_head_gain.txt": "1.0",
        "re6vr_head_range.txt": "60",
        "re6vr_head_deadzone.txt": "1.5",    # delta mode only
        "re6vr_view.txt": "off",
        "re6vr_render_probe.txt": "1",       # read-only: displayCount / Stereo flag evidence
        # Stereo groundwork. Inert while ipd = 0. Set ipd to ~0.063 AND eye_index to 0 or 1 for a
        # single-eye test: the game camera shifts sideways by half the IPD, which proves the eye
        # offset reaches the renderer before any dual-pass work is attempted.
        "re6vr_ipd.txt": "0",
        "re6vr_eye_index.txt": "-1",
        # 1 = the render phase runs a second time with the right eye selected. An experiment: both
        # passes draw into the same target today, so it proves "the engine renders twice per frame"
        # rather than producing a stereo picture.
        "re6vr_stereo.txt": "0",
    },
    "fovmatch": {                            # single-eye, with the game's fov matched to the panel
        # The measured state (2026-09-25): the game renders 37 deg vertical, the virtual panel subtends
        # 59.8 x 35.8 deg, and the headset's own per-eye view is 78.2 x 81.4 deg. The picture is shown at
        # the angle the PANEL subtends, so the game's fov has to equal the panel's vertical subtense or
        # the picture is stretched. 35.8 is that number - and it is a starting point, not a fact: the
        # proxy now prints "fov match: game renders X | panel declared Y | match Z" once a second, and Z
        # is the thing to read. Tune re6vr_fov.txt until Z sits at 1.00.
        #
        # No stereo of any kind: eight runs proved that every "two eyes per frame" route crashes the
        # driver on this machine. This config is about the ONE picture looking right.
        "re6vr_steer.txt": "head",
        "re6vr_head_method.txt": "1",
        "re6vr_head_absolute.txt": "1",
        "re6vr_head_tripod.txt": "1",
        "re6vr_head_gain.txt": "1.0",
        "re6vr_head_range.txt": "60",
        "re6vr_head_deadzone.txt": "1.5",
        "re6vr_view.txt": "off",
        "re6vr_render_probe.txt": "1",
        "re6vr_rt_probe.txt": "0",
        "re6vr_fov.txt": "35.8",
        "re6vr_ipd.txt": "0",
        "re6vr_eye_index.txt": "-1",
        "re6vr_stereo.txt": "0",
        "re6vr_stereo_pass2.txt": "0",
    },
    "redirect": {                            # the working stereo mode: engine draws both eyes, we bind the targets
        # Both engine passes are redirected into a texture of ours (measured: the engine binds its
        # render target outside the render phase and never inside it, so one binding per pass sticks).
        # The compositor copies ONE eye per frame, alternating, so the per-frame readback stays at the
        # level the verified single-texture build has always run at.
        "re6vr_steer.txt": "head",
        "re6vr_head_method.txt": "1",
        "re6vr_head_absolute.txt": "1",
        "re6vr_head_tripod.txt": "1",
        "re6vr_head_gain.txt": "1.0",
        "re6vr_head_range.txt": "60",
        "re6vr_head_deadzone.txt": "1.5",
        "re6vr_view.txt": "off",
        "re6vr_render_probe.txt": "1",
        "re6vr_rt_probe.txt": "0",
        "re6vr_ipd.txt": "0.063",
        "re6vr_eye_index.txt": "0",
        "re6vr_stereo.txt": "1",
        "re6vr_stereo_pass2.txt": "1",       # the second engine pass IS the point here; it is redirected
    },
    "probe": {                               # READ-ONLY observation: no stereo, no eye offset, no capture
        # What it answers: which surface the engine's render phase actually draws into. That decides
        # whether one pass can be redirected to a texture of our own (a SetRenderTarget detour), which
        # would give two eyes two separately-capturable pictures with only ONE readback per frame - the
        # readback being where every crash so far landed.
        #
        # Everything else stays inert: ipd 0 / eye_index -1 / stereo 0 means no per-eye surfaces are
        # created, no capture runs and the camera is not offset. The run is safe to play normally.
        "re6vr_steer.txt": "head",
        "re6vr_head_method.txt": "1",
        "re6vr_head_absolute.txt": "1",
        "re6vr_head_tripod.txt": "1",
        "re6vr_head_gain.txt": "1.0",
        "re6vr_head_range.txt": "60",
        "re6vr_head_deadzone.txt": "1.5",
        "re6vr_view.txt": "off",
        "re6vr_render_probe.txt": "1",
        "re6vr_rt_probe.txt": "1",           # read-only render-target observation
        "re6vr_ipd.txt": "0",
        "re6vr_eye_index.txt": "-1",
        "re6vr_stereo.txt": "0",
        "re6vr_stereo_pass2.txt": "0",
    },
    "stereo": {                              # dual-pass stereo: two renders per frame, one per eye
        # Step 1 of the plan in _work/HANDOFF-stereo-vr.md: prove that the engine really renders
        # twice per frame with a different eye each time. The eye offset has to be armed for the two
        # passes to differ at all, and eye_index = 0 puts pass 1 on the left eye explicitly (pass 2
        # always selects the right eye itself).
        "re6vr_steer.txt": "head",
        "re6vr_head_method.txt": "1",
        "re6vr_head_absolute.txt": "1",
        "re6vr_head_tripod.txt": "1",
        "re6vr_head_gain.txt": "1.0",
        "re6vr_head_range.txt": "60",
        "re6vr_head_deadzone.txt": "1.5",
        "re6vr_view.txt": "off",
        "re6vr_render_probe.txt": "1",
        "re6vr_ipd.txt": "0.063",
        "re6vr_eye_index.txt": "0",
        "re6vr_stereo.txt": "1",
        # The second ENGINE pass per frame. 0 = the safe one-eye-per-frame mode (the eye alternates,
        # every capture happens at EndScene). 1 = the dual-pass experiment, which killed four runs on
        # 2026-09-25 with the same driver fault - only set it to reproduce that on purpose.
        "re6vr_stereo_pass2.txt": "0",
    },
    "delta": {                               # the older per-step drag model, for A/B
        "re6vr_steer.txt": "head",
        "re6vr_head_method.txt": "1",
        "re6vr_head_absolute.txt": "0",
        "re6vr_head_tripod.txt": "1",
        "re6vr_head_gain.txt": "1.0",
        "re6vr_head_range.txt": "60",
        "re6vr_head_deadzone.txt": "1.5",
        "re6vr_view.txt": "off",
    },
    "observe": {                             # read-only: no camera writes at all
        "re6vr_steer.txt": "observe",
        "re6vr_head_absolute.txt": "0",
        "re6vr_view.txt": "off",
    },
    "off": {                                 # nothing armed
        "re6vr_steer.txt": "off",
        "re6vr_head_absolute.txt": "0",
        "re6vr_view.txt": "off",
    },
}


def md5(path: str) -> str:
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="head", choices=sorted(CONFIGS))
    ap.add_argument("--gain", default=None)
    ap.add_argument("--range", dest="range_deg", default=None)
    ap.add_argument("--tripod", default=None)
    ap.add_argument("--ipd", default=None, help="eye separation in metres (0 = off)")
    ap.add_argument("--print", dest="show_only", action="store_true")
    args = ap.parse_args()

    markers = dict(CONFIGS[args.config])
    if args.gain is not None:
        markers["re6vr_head_gain.txt"] = args.gain
    if args.range_deg is not None:
        markers["re6vr_head_range.txt"] = args.range_deg
    if args.tripod is not None:
        markers["re6vr_head_tripod.txt"] = args.tripod
    if args.ipd is not None:
        markers["re6vr_ipd.txt"] = args.ipd

    print("[config] %s" % args.config)
    for name in sorted(markers):
        print("   %-28s = %s" % (name, markers[name]))
    if args.show_only:
        return 0

    os.makedirs(WORK, exist_ok=True)
    for name, value in markers.items():
        with open(os.path.join(WORK, name), "w", encoding="ascii") as fh:
            fh.write(value + "\n")
    with open(os.path.join(BUILD, "re6vr_logdir.txt"), "w", encoding="utf-8") as fh:
        fh.write(WORK)

    src = os.path.join(BUILD, "d3d9.dll")
    dst = os.path.join(GAME, "d3d9.dll")
    if not os.path.exists(src):
        print("[config] ERROR: %s missing - run scripts\\build.bat first" % src)
        return 1
    shutil.copy2(src, dst)

    same = md5(src) == md5(dst)
    print("[config] deployed %d bytes to %s" % (os.path.getsize(dst), GAME))
    print("[config] hashes match: %s (%s)" % (same, md5(dst)))
    print("[config] log directory: %s" % open(os.path.join(BUILD, "re6vr_logdir.txt")).read())
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
