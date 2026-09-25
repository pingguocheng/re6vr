#!/usr/bin/env python3
"""Stereo-render investigation: step 7 - the actual IDirect3DDevice9 call sites.

An earlier pass (sr_vslot3.py) treated every `mov reg,[base+disp]; call reg` as a device
vtable call, which is wrong: that pattern is the *engine's own* virtual dispatch (there are
thousands of them). A real d3d9 call has a **two-level** shape, because the engine holds the
device somewhere and reloads the vtable from it:

    mov  ecx, [<wrapper>+0xNN]     ; the raw IDirect3DDevice9*
    ...
    mov  eax, [ecx]                ; its vtable
    mov  edx, [eax+0x44]           ; slot 17 = Present
    call edx

This scan looks for: `mov reg2,[reg1]` (a vtable load, disp 0) followed by
`mov reg3,[reg2+disp]` followed by `call reg3`. That is the shape MSVC emits for a COM call
where the vtable pointer is re-read, which is what this binary does for d3d9 (the project's
existing Present patch proves the device pointer is fetched per call).
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


def dname(disp):
    if disp % 4:
        return None
    s = disp // 4
    if 0 <= s < len(DEV):
        return DEV[s]
    return None


def load_all(force=False):
    if not force and os.path.exists(PICKLE):
        with open(PICKLE, "rb") as fh:
            return pickle.load(fh)
    from disasm_lib import analyze
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
    print("functions %d  instructions %d" % (len(funcs), sum(len(r) for _, _, r in funcs)))

    # shape: mov r2,[r1]  (disp 0, mem->reg) ; mov r3,[r2+d] ; call r3
    three = defaultdict(list)
    two = defaultdict(list)
    for start, end, rows in funcs:
        by_va = {r[0]: r for r in rows}
        for va, mn, ops, is_call, is_jmp, ln in rows:
            n1 = by_va.get(va + ln)
            if n1 is None:
                continue
            va1, mn1, ops1, c1, j1, ln1 = n1
            n2 = by_va.get(va1 + ln1)
            if n2 is None:
                continue
            va2, mn2, ops2, c2, j2, ln2 = n2
            # three-instruction shape: mov r2,[r1] / mov r3,[r2+d] / call r3
            if (mn == "mov" and mn1 == "mov" and (c2 or j2)
                    and len(ops) == 2 and len(ops1) == 2 and len(ops2) == 1):
                if (ops[0][0] == "reg" and ops[1][0] == "mem" and ops[1][3] is None
                        and ops[1][5] == 0 and ops[1][4] == 0):
                    r1 = ops[1][2]
                    d2 = ops1[1][3]
                    if (ops1[0][0] == "reg" and ops1[1][0] == "mem"
                            and ops1[1][2] == ops[0][0] and d2 is None
                            and 0x10 <= ops1[1][4] <= 0x400):
                        if ops2[0][0] == "reg" and ops2[0][1] == ops1[0][1]:
                            three[ops1[1][4]].append((start, va, va1, va2,
                                                      "mov %s,[%s] / mov %s,[%s+0x%X] / %s %s"
                                                      % (ops[0][1], r1, ops1[0][1], ops[0][1],
                                                         ops1[1][4],
                                                         "call" if c2 else "jmp", ops2[0][1])))
            # two-instruction shape with a slot: mov r2,[r1+d] / call r2
            if (mn == "mov" and (c1 or j1) and len(ops) == 2 and len(ops1) == 1):
                if (ops[0][0] == "reg" and ops[1][0] == "mem" and ops[1][3] is None
                        and 0x10 <= ops[1][4] <= 0x400):
                    if ops1[0][0] == "reg" and ops1[0][1] == ops[0][1]:
                        two[ops[1][4]].append((start, va, va1,
                                               "mov %s,[%s+0x%X] / %s %s"
                                               % (ops[0][1], ops[1][2] or "?",
                                                  ops[1][4], "call" if c1 else "jmp",
                                                  ops1[0][1])))

    print()
    print("== TWO-level shape (vtable reload then call) by slot ==")
    for d in sorted(three):
        nm = dname(d)
        if nm is None:
            continue
        print("  +0x%03X slot %-3d %-28s %4d site(s)" % (d, d // 4, nm, len(three[d])))
    print()
    for d in sorted(three):
        if dname(d) not in ("Present", "EndScene", "BeginScene", "Clear", "Reset",
                            "GetBackBuffer", "SetRenderTarget", "GetRenderTarget"):
            continue
        print("== +0x%03X %s : %d site(s) ==" % (d, dname(d), len(three[d])))
        for start, va, va1, va2, txt in sorted(three[d]):
            print("    %08X in fn %08X .. %08X" % (va, start, 0))
            print("        %08X %s" % (va1, txt))
    return 0


if __name__ == "__main__":
    sys.exit(main())
