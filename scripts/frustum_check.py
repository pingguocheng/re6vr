#!/usr/bin/env python3
"""Reference model of draw_quad()'s frustum, to check it against the eye buffers.

Reimplements the panel/view/projection maths from src/openxr_bridge.cpp exactly,
so a change to the frustum can be evaluated here (instantly) before it is built
into the DLL and measured in the headset.

Usage: python scripts/frustum_check.py
"""
import math


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)]
            for r in range(4)]


def transform(m, v):
    return [sum(m[r][k] * v[k] for k in range(4)) for r in range(4)]


def build(panel_center, panel_right, panel_up, panel_fwd, eye, panel_w, panel_h,
          vp_w, vp_h, fix_aspect):
    """Return the NDC corners of the panel as the shader would see them."""
    world = [[panel_right[0], panel_up[0], panel_fwd[0], panel_center[0]],
             [panel_right[1], panel_up[1], panel_fwd[1], panel_center[1]],
             [panel_right[2], panel_up[2], panel_fwd[2], panel_center[2]],
             [0.0, 0.0, 0.0, 1.0]]

    # Eye looks straight down -Z with the panel's own basis (identity rotation),
    # which is what ipd_scale = 0 converges to.
    right, up, fwd = [1, 0, 0], [0, 1, 0], [0, 0, -1]
    view = [[right[0], up[0], fwd[0], -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2])],
            [right[1], up[1], fwd[1], -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2])],
            [right[2], up[2], fwd[2], -(fwd[0] * eye[0] + fwd[1] * eye[1] + fwd[2] * eye[2])],
            [0.0, 0.0, 0.0, 1.0]]

    # Panel centre in view space (column-major convention as in the C++).
    vp = [sum(world[r][c] * 0 for c in range(4)) for r in range(4)]
    pc = panel_center
    vx = pc[0] * view[0][0] + pc[1] * view[0][1] + pc[2] * view[0][2] + view[0][3]
    vy = pc[0] * view[1][0] + pc[1] * view[1][1] + pc[2] * view[1][2] + view[1][3]
    vz = pc[0] * view[2][0] + pc[1] * view[2][1] + pc[2] * view[2][2] + view[2][3]
    vp = [vx, vy, vz]

    panel_z = max(vp[2], 0.25)
    tan_l = (vp[0] - panel_w * 0.5) / panel_z
    tan_r = (vp[0] + panel_w * 0.5) / panel_z
    tan_d = (vp[1] - panel_h * 0.5) / panel_z
    tan_u = (vp[1] + panel_h * 0.5) / panel_z

    applied = False
    if fix_aspect:
        vp_aspect = vp_w / vp_h
        w = tan_r - tan_l
        h = tan_u - tan_d
        off_x = (tan_l + tan_r) * 0.5
        off_y = (tan_d + tan_u) * 0.5
        src_aspect = panel_w / panel_h
        if w / h > src_aspect:
            h = w / src_aspect
        else:
            w = h * src_aspect
        tan_l = off_x - w * 0.5
        tan_r = off_x + w * 0.5
        tan_d = off_y - h * 0.5
        tan_u = off_y + h * 0.5
        # Fit that region inside the viewport without shrinking the panel.
        vp_w_tan = vp_aspect * h
        if w < vp_w_tan:
            w = vp_w_tan
            applied = True
        elif w > vp_w_tan:
            h = w / vp_aspect
            applied = True
        tan_l = off_x - w * 0.5
        tan_r = off_x + w * 0.5
        tan_d = off_y - h * 0.5
        tan_u = off_y + h * 0.5

        proj = [[0.0] * 4 for _ in range(4)]
        proj[0] = [0.0, 0.0, 0.0, 0.0]
        proj[1] = [0.0, 0.0, 0.0, 0.0]
        proj[2] = [0.0, 0.0, 0.0, 0.0]
        proj[3] = [0.0, 0.0, 0.0, 0.0]
        proj[0][0] = 2.0 / (tan_r - tan_l)
        proj[1][1] = 2.0 / (tan_u - tan_d)
        proj[0][2] = -(tan_r + tan_l) / (tan_r - tan_l)
        proj[1][2] = -(tan_u + tan_d) / (tan_u - tan_d)
    else:
        proj = [[0.0] * 4 for _ in range(4)]
        proj[0][0] = 2.0 / (tan_r - tan_l)
        proj[1][1] = 2.0 / (tan_u - tan_d)
        proj[0][2] = -(tan_r + tan_l) / (tan_r - tan_l)
        proj[1][2] = -(tan_u + tan_d) / (tan_u - tan_d)

    vp_mat = mat_mul(world, view)
    vpm = mat_mul(vp_mat, proj)

    hw, hh = panel_w * 0.5, panel_h * 0.5
    corners = [(-hw, +hh), (+hw, +hh), (+hw, -hh), (-hw, -hh)]
    out = []
    for lx, ly in corners:
        wx = lx * world[0][0] + ly * world[1][0] + world[0][3]
        wy = lx * world[0][1] + ly * world[1][1] + world[1][3]
        wz = lx * world[0][2] + ly * world[1][2] + world[2][3]
        v = transform(vpm, [wx, wy, wz, 1.0])
        w = v[3]
        out.append((v[0] / w if w else 0.0, v[1] / w if w else 0.0, w))
    return out, (tan_l, tan_r, tan_d, tan_u)


def report(tag, corners, tans):
    xs = [c[0] for c in corners]
    ys = [c[1] for c in corners]
    print(f"  {tag}")
    print(f"    tan l/r/d/u = {tans[0]:+.4f} {tans[1]:+.4f} {tans[2]:+.4f} {tans[3]:+.4f}"
          f"  (width {tans[1]-tans[0]:.4f}, height {tans[3]-tans[2]:.4f},"
          f" aspect {(tans[1]-tans[0])/(tans[3]-tans[2]):.3f})")
    print(f"    NDC x {min(xs):+.3f}..{max(xs):+.3f}   y {min(ys):+.3f}..{max(ys):+.3f}")
    print(f"    on-screen fraction: x {min(1.0,(max(xs)-min(xs))/2.0)*100:.1f}%"
          f"  y {min(1.0,(max(ys)-min(ys))/2.0)*100:.1f}%")
    inside = all(-1.001 <= c[0] <= 1.001 and -1.001 <= c[1] <= 1.001 for c in corners)
    print(f"    corners inside the viewport: {inside}")


def main():
    # The measured game values (re6vr.log, 2026-09-22 20:22).
    panel_w, panel_h = 2.90, 2.90 * 9.0 / 16.0
    panel_center = (-0.03, -1.12, -4.10)
    panel_right = (1.0, 0.0, 0.0)
    panel_up = (0.0, 1.0, 0.0)
    panel_fwd = (0.0, 0.0, 1.0)
    eye = (-0.03, -1.12, -2.50)          # panel_center - fwd * 1.6
    vp_w, vp_h = 998.0, 2148.0           # per-eye swapchain slice

    print("viewport 998x2148, aspect %.3f; source 16:9 = 1.778" % (vp_w / vp_h))
    print("\nCURRENT code (frustum straight from the panel rectangle):")
    c, t = build(panel_center, panel_right, panel_up, panel_fwd, eye,
                 panel_w, panel_h, vp_w, vp_h, fix_aspect=False)
    report("as built today", c, t)

    print("\nWITH the aspect fix:")
    c, t = build(panel_center, panel_right, panel_up, panel_fwd, eye,
                 panel_w, panel_h, vp_w, vp_h, fix_aspect=True)
    report("with fix", c, t)


if __name__ == "__main__":
    main()
