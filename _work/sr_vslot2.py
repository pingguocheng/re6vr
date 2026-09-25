#!/usr/bin/env python3
"""Stereo-render investigation: step 5 - device-vtable slot call sites, operand based.

Finds `mov reg,[base(+disp)]` immediately followed by `call reg` / `jmp reg`, where disp is a
plausible IDirect3DDevice9 vtable byte offset. Operand-based (not regex-on-text) so prefixes
and spacing cannot break it, and only over decoded instruction boundaries so an immediate that
happens to contain the bytes cannot produce a false hit.
"""
from __future__ import annotations

import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
sys.path.insert(0, SCRIPTS)

from disasm_lib import analyze  # noqa: E402

CACHE = os.path.join(os.path.dirname(HERE), "_work", "bh6_analysis.json")

# IDirect3DDevice9 slot -> method name (d3d9.h order, 0-based slot index, byte offset = slot*4)
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


def name_of(disp: int):
    if disp % 4:
        return None
    slot = disp // 4
    if 0 <= slot < len(DEV):
        return DEV[slot]
    return None


def main() -> int:
    an = analyze.load_analysis(CACHE, None)
    if an is None:
        return 1
    print("instructions in cache: %d" % len(an.decoded))

    # `mov reg, [base+disp]` where the operand kinds tell us it is a plain 32-bit mov
    pairs = 0
    hits: dict[int, list[tuple[int, str, str, str]]] = defaultdict(list)
    reg_names = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")
    vreg_names = ("xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7")
    for va in sorted(an.decoded):
        ins = an.decoded[va]
        if not ins.ok or ins.mnemonic != "mov" or len(ins.operands) != 2:
            continue
        dst, src = ins.operands
        if dst.kind != "reg" or dst.text not in reg_names:
            continue
        if src.kind != "mem" or src.index is not None or src.base is None:
            continue
        d = src.disp
        if d < 0x10 or d > 0x400:
            continue
        if name_of(d) is None:
            continue
        nxt = an.decoded.get(va + ins.length)
        if nxt is None or not nxt.ok or len(nxt.operands) != 1:
            continue
        if not (nxt.is_call or nxt.is_jmp):
            continue
        tgt = nxt.operands[0]
        if tgt.kind != "reg" or tgt.text != dst.text:
            continue
        pairs += 1
        hits[d].append((va, ins.text(), nxt.text(), "call" if nxt.is_call else "jmp"))
    print("total mov-slot / call-reg pairs: %d" % pairs)
    print()
    for d in sorted(hits):
        print("  +0x%02X slot %-3d %-28s %d site(s)"
              % (d, d // 4, name_of(d), len(hits[d])))
    print()
    print("== sites for the frame-relevant slots ==")
    for d in (68, 84, 88, 92, 64, 456, 108, 72, 500, 576):
        if d not in hits:
            continue
        print("  --- +0x%02X %s ---" % (d, name_of(d)))
        for va, a, b, kind in sorted(hits[d]):
            print("      %08X  %-28s %s" % (va, a, b))
    return 0


if __name__ == "__main__":
    sys.exit(main())
