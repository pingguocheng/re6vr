#!/usr/bin/env python3
"""Stereo-render investigation: step 6 - device-vtable slot call sites, correct iterator.

The earlier attempt iterated `an.decoded`, which `load_analysis` deliberately leaves EMPTY
(instructions decode lazily). Everything must go through `iter_instructions`, exactly as the
project note says. Results are cached as a pickle so later scans are instant.
"""
from __future__ import annotations

import os
import pickle
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
sys.path.insert(0, SCRIPTS)

from disasm_lib import analyze  # noqa: E402

CACHE = os.path.join(os.path.dirname(HERE), "_work", "bh6_analysis.json")
PICKLE = os.path.join(HERE, "_sr_all_ins.pkl")

DEV = [
    "QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem",
    "EvictManagedResources", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode",
    "GetCreationParameters", "SetCursorProperties", "SetCursorPosition", "ShowCursor",
    "CreateAdditionalSwapChain", "GetSwapChain", "GetNumberOfSwapChains", "Reset", "Present",
    "GetBackBuffer", "GetRasterStatus", "SetDialogBoxMode", "BeginScene", "EndScene", "Clear",
    "SetTransform", "GetTransform", "MultiplyTransform", "SetViewport", "GetViewport",
    "SetMaterial", "GetMaterial", "SetLight", "GetLight", "LightEnable", "GetLightEnable",
    "SetClipPlane", "GetClipPlane", "SetRenderState", "GetRenderState", "CreateStateBlock",
    "BeginStateBlock", "EndStateBlock", "SetClipStatus", "GetClipStatus", "GetTexture",
    "SetTexture", "GetTextureStageState", "SetTextureStageState", "GetSamplerState",
    "SetSamplerState", "ValidateDevice", "SetPaletteEntries", "GetPaletteEntries",
    "SetCurrentTexturePalette", "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect",
    "SetSoftwareVertexProcessing", "GetSoftwareVertexProcessing", "SetNPatchMode",
    "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP",
    "DrawIndexedPrimitiveUP", "ProcessVertices", "CreateVertexDeclaration",
    "SetVertexDeclaration", "GetVertexDeclaration", "SetFVF", "GetFVF", "CreateVertexShader",
    "SetVertexShader", "GetVertexShader", "SetVertexShaderConstantF",
    "GetVertexShaderConstantF", "SetVertexShaderConstantI", "GetVertexShaderConstantI",
    "SetVertexShaderConstantB", "GetVertexShaderConstantB", "SetStreamSource",
    "GetStreamSource", "SetStreamSourceFreq", "GetStreamSourceFreq", "SetIndices", "GetIndices",
    "CreatePixelShader", "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF",
    "GetPixelShaderConstantF", "SetPixelShaderConstantI", "GetPixelShaderConstantI",
    "SetPixelShaderConstantB", "GetPixelShaderConstantB", "DrawIndexedPrimitiveUP2",
    "CreateTexture", "GetTexture2", "CreateVolumeTexture", "VolumeTexture", "CreateCubeTexture",
    "GetCubeTexture", "SetPixelShader2", "GetPixelShader2", "SetFVF2", "CreateVertexShader2",
    "SetVertexShader2", "GetVertexShader2", "SetStreamSource2", "GetStreamSource2",
    "CreateRenderTarget", "CreateDepthStencilSurface", "UpdateSurface", "UpdateTexture",
    "GetRenderTargetData", "GetFrontBufferData", "StretchRect", "ColorFill",
    "CreateOffscreenPlainSurface", "SetRenderTarget", "GetRenderTarget",
    "GetDepthStencilSurface", "BeginStateBlock2", "CreateStateBlock2", "EndStateBlock2",
    "Clear2", "SetTransform2", "Present2", "GetBackBuffer2",
]


def name_of(disp):
    if disp % 4:
        return None
    slot = disp // 4
    if 0 <= slot < len(DEV):
        return DEV[slot]
    return None


def load_all(force=False):
    """List of (fn_start, fn_end, [(va, mnemonic, operands_tuple, is_call, is_jmp, length)])"""
    if not force and os.path.exists(PICKLE):
        with open(PICKLE, "rb") as fh:
            return pickle.load(fh)
    an = analyze.load_analysis(CACHE, None)
    out = []
    for f in an.functions.values():
        rows = []
        for va in sorted(f.blocks):
            ins = an.decode_at(va)
            if ins is None or not ins.ok:
                continue
            rows.append((va, ins.mnemonic,
                         tuple((o.kind, o.text, o.base, o.index, o.disp, o.target)
                               for o in ins.operands),
                         ins.is_call, ins.is_jmp, ins.length))
        out.append((f.start, f.end, rows))
    with open(PICKLE, "wb") as fh:
        pickle.dump(out, fh)
    return out


def main() -> int:
    funcs = load_all()
    total = sum(len(r) for _, _, r in funcs)
    print("functions %d  instructions %d" % (len(funcs), total))

    hops = defaultdict(list)
    for start, end, rows in funcs:
        by_va = {r[0]: r for r in rows}
        for va, mn, ops, is_call, is_jmp, ln in rows:
            if mn != "mov" or len(ops) != 2:
                continue
            dk, dt, db, di, dd, dtg = ops[0]
            sk, st, sb, si, sd, stg = ops[1]
            if dk != "reg" or sk != "mem" or si is not None or sb is None:
                continue
            if not (0x10 <= sd <= 0x400):
                continue
            if name_of(sd) is None:
                continue
            nxt = by_va.get(va + ln)
            if nxt is None:
                continue
            _, mn2, ops2, c2, j2, _ = nxt
            if not (c2 or j2) or len(ops2) != 1:
                continue
            if ops2[0][0] != "reg" or ops2[0][1] != dt:
                continue
            hops[sd].append((start, va, "%s %s" % (mn, ", ".join(o[1] for o in ops)),
                             "%s %s" % (mn2, ops2[0][1])))

    print("total slot-loaded-then-called patterns: %d" % sum(len(v) for v in hops.values()))
    print()
    for d in sorted(hops):
        print("  +0x%02X slot %-3d %-28s %4d site(s)"
              % (d, d // 4, name_of(d), len(hops[d])))
    print()
    for d in (68, 84, 88, 92, 64, 456, 108, 72, 500, 576, 420, 424):
        if d not in hops:
            continue
        print("== +0x%02X %s : %d sites ==" % (d, name_of(d), len(hops[d])))
        for start, va, a, b in sorted(hops[d]):
            print("    %08X in fn %08X   %s / %s" % (va, start, a, b))
    return 0


if __name__ == "__main__":
    sys.exit(main())
