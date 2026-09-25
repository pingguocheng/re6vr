#!/usr/bin/env python3
"""Stereo-render investigation: step 8 - what the two-level shape actually looks like.

Rather than guessing the exact call shape, this dumps the instruction window around every
`mov reg,[reg2+disp]` with disp in the device-vtable range that is followed within 4
instructions by an indirect call/jmp through that same register - printing all the surrounding
instructions so the real shape can be read off instead of assumed.
"""
from __future__ import annotations

import os
import pickle
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
sys.path.insert(0, SCRIPTS)

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


def load_all():
    with open(PICKLE, "rb") as fh:
        return pickle.load(fh)


def render(rows_by_va, va):
    r = rows_by_va.get(va)
    if r is None:
        return None
    return "%08X  %-24s %s" % (r[0], r[1], ", ".join(o[1] for o in r[2]))


def main() -> int:
    funcs = load_all()
    n = 0
    for start, end, rows in funcs:
        by_va = {r[0]: r for r in rows}
        ordered = [r[0] for r in rows]
        for i, (va, mn, ops, is_call, is_jmp, ln) in enumerate(rows):
            if mn != "mov" or len(ops) != 2:
                continue
            dk, dt, db, di, dd, dtg = ops[0]
            sk, st, sb, si, sd, stg = ops[1]
            if dk != "reg" or sk != "mem" or si is not None:
                continue
            if not (0x10 <= sd <= 0x400) or sd % 4:
                continue
            # look ahead up to 4 instructions for `call dt`
            found = None
            for j in range(i + 1, min(i + 5, len(rows))):
                r2 = rows[j]
                if (r2[3] or r2[4]) and len(r2[2]) == 1 and r2[2][0][0] == "reg" \
                        and r2[2][0][1] == dt:
                    found = r2
                    break
            if found is None:
                continue
            n += 1
            slot = sd // 4
            nm = DEV[slot] if slot < len(DEV) else "?"
            print("=== site %08X  fn %08X  disp 0x%X (%s) ===" % (va, start, sd, nm))
            lo = max(0, i - 4)
            hi = min(len(rows), i + 5)
            for k in range(lo, hi):
                mark = ">>" if k == i else ("**" if rows[k][0] == found[0] else "  ")
                print("  %s %s" % (mark, render(by_va, rows[k][0])))
            print()
    print("sites: %d" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
