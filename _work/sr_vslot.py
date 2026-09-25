#!/usr/bin/env python3
"""Stereo-render investigation: step 4 - device-vtable slot call sites.

Finds every `mov reg,[base+disp]; call reg` / `jmp reg` pair where disp is one of the
IDirect3DDevice9 vtable byte offsets (slot*4). This is the form MSVC emits for COM calls in
this binary (the memory-operand form `call [reg+disp]` returns zero hits - see the project's
notes). Slot byte offsets for IDirect3DDevice9 (d3d9.h order):

  0  QueryInterface       1  AddRef             2  Release
  3  TestCooperativeLevel 4  GetAvailableTextureMem
  5  EvictManagedResources 6 GetDirect3D      7  GetDeviceCaps
  8  GetDisplayMode       9  GetCreationParameters
  10 SetCursorProperties  11 SetCursorPosition 12 ShowCursor  13 CreateAdditionalSwapChain
  14 GetSwapChain         15 GetNumberOfSwapChains 16 Reset   17 Present
  18 GetBackBuffer        19 GetRasterStatus   20 SetDialogBoxMode
  21 BeginScene           22 EndScene         23 Clear     24 SetTransform
  25 GetTransform         26 MultiplyTransform 27 SetViewport 28 GetViewport
  29 SetMaterial          30 GetMaterial      31 SetLight  32 GetLight
  33 LightEnable          34 GetLightEnable   35 SetClipPlane 36 GetClipPlane
  37 SetRenderState       38 GetRenderState   39 CreateStateBlock 40 BeginStateBlock
  41 EndStateBlock        42 SetClipStatus    43 GetClipStatus
  44 GetTexture           45 SetTexture       46 GetTextureStageState 47 SetTextureStageState
  48 GetSamplerState       49 SetSamplerState   50 ValidateDevice 51 SetPaletteEntries
  52 GetPaletteEntries     53 SetCurrentTexturePalette 54 GetCurrentTexturePalette
  55 SetScissorRect        56 GetScissorRect    57 SetSoftwareVertexProcessing
  58 GetSoftwareVertexProcessing 59 SetNPatchMode 60 GetNPatchMode
  61 DrawPrimitive         62 DrawIndexedPrimitive 63 DrawPrimitiveUP
  64 DrawIndexedPrimitiveUP 65 ProcessVertices 66 CreateVertexDeclaration
  67 SetVertexDeclaration  68 GetVertexDeclaration 69 SetFVF 70 GetFVF
  71 CreateVertexShader    72 SetVertexShader  73 GetVertexShader
  74 SetVertexShaderConstantF 75 GetVertexShaderConstantF
  76 SetVertexShaderConstantI 77 GetVertexShaderConstantI
  78 SetVertexShaderConstantB 79 GetVertexShaderConstantB
  80 SetStreamSource       81 GetStreamSource  82 SetStreamSourceFreq 83 GetStreamSourceFreq
  84 SetIndices            85 GetIndices       86 GetVertexDeclaration  (dup)
  87 CreatePixelShader     88 SetPixelShader   89 GetPixelShader
  90 SetPixelShaderConstantF ...
  94 SetPixelShaderConstantB
  95 CreateTexture         96 GetTexture(2)    97 CreateVolumeTexture
  99 CreateCubeTexture ...
  105 CreateRenderTarget    106 CreateDepthStencilSurface
  107 UpdateSurface        108 UpdateTexture   109 GetRenderTargetData
  110 GetFrontBufferData   111 StretchRect     112 ColorFill
  113 CreateOffscreenPlainSurface
  114 SetRenderTarget       115 GetRenderTarget 116 GetDepthStencilSurface
  117 BeginStateBlock(dup) ...
  119 Clear(dup) ...
  123 SetRenderState(dup)
  124 BeginScene
  125 EndScene
  126 ForEachShader ...
  143 GetAvailableTextureMem
  144 Present
  145 GetBackBuffer
"""
from __future__ import annotations

import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(os.path.dirname(HERE), "scripts")
sys.path.insert(0, SCRIPTS)

from disasm_lib import pe, analyze, x86  # noqa: E402

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe"
CACHE = os.path.join(os.path.dirname(HERE), "_work", "bh6_analysis.json")

SLOT_NAMES = {
    0x00: "QueryInterface", 0x08: "TestCooperativeLevel", 0x18: "GetDirect3D",
    0x1C: "GetDeviceCaps", 0x28: "GetCreationParameters", 0x30: "SetCursorProperties",
    0x34: "SetCursorPosition", 0x38: "ShowCursor", 0x3C: "CreateAdditionalSwapChain",
    0x40: "GetSwapChain", 0x44: "GetNumberOfSwapChains", 0x44 + 0: "GetNumberOfSwapChains",
    0x48: "/Reset(0x48?)", 0x4C: "Present?", 0x50: "GetBackBuffer?",
}
# exact byte offsets = slot*4
SLOTS = {
    "QueryInterface": 0, "AddRef": 4, "Release": 8, "TestCooperativeLevel": 12,
    "GetAvailableTextureMem": 16, "EvictManagedResources": 20, "GetDirect3D": 24,
    "GetDeviceCaps": 28, "GetDisplayMode": 32, "GetCreationParameters": 36,
    "SetCursorProperties": 40, "SetCursorPosition": 44, "ShowCursor": 48,
    "CreateAdditionalSwapChain": 52, "GetSwapChain": 56, "GetNumberOfSwapChains": 60,
    "Reset": 64, "Present": 68, "GetBackBuffer": 72, "GetRasterStatus": 76,
    "SetDialogBoxMode": 80, "BeginScene": 84, "EndScene": 88, "Clear": 92,
    "SetTransform": 96, "GetTransform": 100, "MultiplyTransform": 104, "SetViewport": 108,
    "GetViewport": 112, "SetMaterial": 116, "GetMaterial": 120, "SetLight": 124,
    "GetLight": 128, "LightEnable": 132, "GetLightEnable": 136, "SetClipPlane": 140,
    "GetClipPlane": 144, "SetRenderState": 148, "GetRenderState": 152,
    "CreateStateBlock": 156, "BeginStateBlock": 160, "EndStateBlock": 164,
    "SetClipStatus": 168, "GetClipStatus": 172, "GetTexture": 176, "SetTexture": 180,
    "GetTextureStageState": 184, "SetTextureStageState": 188, "GetSamplerState": 192,
    "SetSamplerState": 196, "ValidateDevice": 200, "SetPaletteEntries": 204,
    "GetPaletteEntries": 208, "SetCurrentTexturePalette": 212,
    "GetCurrentTexturePalette": 216, "SetScissorRect": 220, "GetScissorRect": 224,
    "SetSoftwareVertexProcessing": 228, "GetSoftwareVertexProcessing": 232,
    "SetNPatchMode": 236, "GetNPatchMode": 240, "DrawPrimitive": 244,
    "DrawIndexedPrimitive": 248, "DrawPrimitiveUP": 252, "DrawIndexedPrimitiveUP": 256,
    "ProcessVertices": 260, "CreateVertexDeclaration": 264, "SetVertexDeclaration": 268,
    "GetVertexDeclaration": 272, "SetFVF": 276, "GetFVF": 280,
    "CreateVertexShader": 284, "SetVertexShader": 288, "GetVertexShader": 292,
    "SetVertexShaderConstantF": 296, "GetVertexShaderConstantF": 300,
    "SetVertexShaderConstantI": 304, "GetVertexShaderConstantI": 308,
    "SetVertexShaderConstantB": 312, "GetVertexShaderConstantB": 316,
    "SetStreamSource": 320, "GetStreamSource": 324, "SetStreamSourceFreq": 328,
    "GetStreamSourceFreq": 332, "SetIndices": 336, "GetIndices": 340,
    "CreatePixelShader": 348, "SetPixelShader": 352, "GetPixelShader": 356,
    "SetPixelShaderConstantF": 360, "GetPixelShaderConstantF": 364,
    "SetPixelShaderConstantI": 368, "GetPixelShaderConstantI": 372,
    "SetPixelShaderConstantB": 376, "GetPixelShaderConstantB": 380,
    "CreateTexture": 380, "GetTexture2": 384,
    "CreateRenderTarget": 420, "CreateDepthStencilSurface": 424,
    "UpdateSurface": 428, "UpdateTexture": 432, "GetRenderTargetData": 436,
    "GetFrontBufferData": 440, "StretchRect": 444, "ColorFill": 448,
    "CreateOffscreenPlainSurface": 452, "SetRenderTarget": 456,
    "GetRenderTarget": 460, "GetDepthStencilSurface": 464,
    "BeginScene2": 496, "EndScene2": 500, "Present2": 576, "GetBackBuffer2": 580,
}
# Note: this table is only used for labelling; the scan itself is driven by the numeric
# displacement and prints the best-known name for it.

interesting = {68: "Present", 88: "EndScene", 84: "BeginScene", 92: "Clear", 64: "Reset",
               72: "GetBackBuffer", 108: "SetViewport", 456: "SetRenderTarget",
               500: "EndScene(dup)", 576: "Present(dup)"}

img = pe.Image.load(EXE)
an = analyze.load_analysis(CACHE, EXE)

# Scan raw bytes of .text for the two-instruction sequence:
#   mov reg,[base(+disp)]   (8B /r, mod=10 => 8B 8x/9x/Ax/... , or mod=01 with disp8)
#   call reg / jmp reg      (FF D0+r / FF E0+r)
# We decode with disasm_lib on instruction boundaries instead of scanning bytes, to avoid
# the "false hit inside an immediate" trap the project already recorded.
print("== call/jmp through [reg+disp] where disp looks like a device vtable slot ==")
hits: dict[int, list[tuple[int, str, str]]] = defaultdict(list)
n_pairs = 0
for va in sorted(an.decoded):
    ins = an.decoded[va]
    if not ins.ok:
        continue
    txt = ins.text()
    m = re.match(r"^mov\s+(\w+),\s*\[(\w+)(?:\+(0x[0-9a-fA-F]+))?\]$", txt)
    if not m:
        continue
    reg, base, disp = m.group(1), m.group(2), m.group(3)
    if reg == base:
        continue
    if disp is None:
        continue
    d = int(disp, 16)
    if d > 0x400:
        continue
    # look at the next decoded instruction (usually the very next one)
    nxt = None
    for cand in (va + ins.length,):
        nxt = an.decoded.get(cand)
    if nxt is None or not nxt.ok:
        continue
    t2 = nxt.text()
    if t2 in ("call %s" % reg, "jmp %s" % reg):
        n_pairs += 1
        hits[d].append((va, txt, t2))
print("  total mov-slot;call-reg pairs: %d" % n_pairs)
print()
for d in sorted(hits):
    nm = SLOT_NAMES.get(d, "")
    for name, off in SLOTS.items():
        if off == d:
            nm = name
            break
    print("  disp +0x%02X (slot %d) %s: %d site(s)" % (d, d // 4, nm, len(hits[d])))
