# RE6 stereo render — the engine's frame boundary, and whether one frame can draw two views

Offline static report, 2026-09-25. Image: `C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe`
(PE32, image base `0x400000`, DX9, MT Framework 2.x). **The game was never launched for this work.**
Nothing under `src\` was touched; scratch scripts are in `_work\sr_*.py` (UTF-8, read-only over the exe).

Tooling: `scripts\re6dis.py` (with `--va`), `scripts\disasm_lib` (`pe`, `x86`, `analyze`). Every
address was read out of the binary. Claims are tagged **MEASURED** (bytes/listing quoted, or a
count over the whole image) or **INFERRED**.

---

## 0. The answer, in the form the C++ hook needs

> **1. `IDirect3DDevice9::Present` is called from exactly one place in the image: VA
> `0x00F41D59` (RVA `0xB41D59`), inside the engine's frame-end function `0x00F41C20`.**
> `__thiscall`, `ecx` = the `sRender` singleton (`0x0186E8BC`), no stack arguments.
>
> ```asm
> 00F41D45  mov  eax,[esi+0x100]     ; esi = sRender, +0x100 = IDirect3DDevice9*
> 00F41D4B  mov  edx,[eax]
> 00F41D4D  push 0 / 0 / 0 / 0
> 00F41D55  push eax
> 00F41D56  mov  eax,[edx+0x44]      ; slot 17 = Present
> 00F41D59  call eax
> ```
>
> **2. The frame that drives it is `0x00511D70`** (the application object's method; `this` =
> app, 0x1140-byte stack frame, vtable slot 6 of vtable `0x0151F670`). It does the message pump,
> the per-subsystem ticks, the render phase, then calls `0x00F41C20` at **`0x00512430`**.
>
> **3. The engine already has a multi-pass render loop: `0x00F3EA30` (the `sRender` render phase,
> vtable slot 6) renders the whole scene once per DISPLAY**
> (`for (i = 1; i < sRender->displayCount; ++i) { camera update(i); draw(i); }`),
> and `0x00F3ECB0..0x00F3ED38` is that loop. Today `displayCount` is 1 (written once, at
> `0x00F4301E`, in the device-initialisation path), so one pass is drawn.
>
> **4. The engine also has a per-VIEW loop that walks 8 camera slots per frame** (`0x00EFFA30`),
> but that loop only clips the render region per slot — it sets up *where* each of the 8 views
> would go on screen, and hands the clipped rectangles to the render context. Which camera each
> view uses is decided from the slot record's camera pointer.

So the answer to "how can the engine be made to render twice per frame" has two independent,
engine-provided seams:

* **Seam A (whole-scene re-render, one camera per pass):** the display loop in `0x00F3EA30`.
  Raising the display count and supplying a second swap chain makes the engine render the scene
  twice per frame, with the camera index passed to the camera update (`push ebx` at
  `0x00F3ECCF`). This is the display/split-screen mechanism.
* **Seam B (per-view regions, all views in one scene pass):** `0x00EFFA30` walks the 8
  `sBioCamera` slot records and derives each view's screen rectangle from that view's own camera
  object. Populate a second slot and the view setup covers two regions.

For the mod's purpose (two eyes, two pictures, captured separately) **Seam A is the right shape**
and **Seam B is the cheap first experiment**; see §5 for exactly what to write, and §6 for the
hook plan. Both are far better anchors than anything on the `mViewportCamera` path.

---

## 1. Who calls Present

### 1.1 RE6's d3d9 surface is tiny (MEASURED)

```
015023F8  d3d9.dll      D3DPERF_SetOptions
015023FC  d3d9.dll      D3DPERF_GetStatus
01502400  d3d9.dll      Direct3DCreate9
01502408  d3dx9_43.dll  D3DXSaveTextureToFileA
0150240C  d3dx9_43.dll  D3DXGetShaderOutputSemantics
01502410  d3dx9_43.dll  D3DXGetShaderConstantTable
```

`Present` is **not imported**, so it can only be reached through the device's vtable. Each import
is reached through a `jmp dword ptr [IAT]` thunk placed by the linker inside `.text`
(`0x013E9D76` = `Direct3DCreate9`, MEASURED: the thunk block is at `0x013E9D5E..0x013E9D90` and
is the only place those IAT dwords appear in code).

### 1.2 The device lives at `[sRender+0x100]` (MEASURED)

```asm
00F42D60  mov  dword ptr [esi+0x98],2
00F42D6A  call 0x013E9D76                ; d3d9.dll!Direct3DCreate9
00F42D6F  mov  [esi+0x108],eax           ; esi = 0x186E8BC = sRender singleton -> IDirect3D9*
00F42D79  push 0x16E625C                 ; "ERR05 : Failed to initialize DirectX9."
```

```asm
00F42770  (a factory/idle helper)
00F42781  mov  eax,[ebx+0x108]           ; IDirect3D9*
00F42798  mov  ecx,[eax]
00F4279A  mov  edx,[ecx+0x18]            ; IDirect3D9 slot 6 = GetAdapterCount
00F42DC1  mov  eax,[ecx+0x20]            ; slot 8  = GetAdapterDisplayMode
00F42FB3..00F4305E  the device creation: mov eax,[esi+0x108] / mov edx,[eax] /
                    mov edx,[edx+0x40] / ... call edx     ; IDirect3D9 slot 16 = CreateDevice
```

The `IDirect3DDevice9*` itself is at `[sRender+0x100]` (MEASURED at `0x00F41D45`, `0x00F4219A`,
`0x00F422F6`, `0x00F42305`) — one dword before the `IDirect3D9*`. `sRender`'s singleton
address is `0x0186E8BC`; its vtable is `0x016E5E18`, proved by slot 3 = the ctor `0x00F3F0B0`
which registers `sRender`'s 42 fields (MEASURED via `re6dis.py props sRender`).

The strings this whole area uses name every call (MEASURED, `.rdata` dump):

```
016E5E90  "mDisplay[i].pSwapChain->GetBackBuffer( 0, D3DBACKBUFFER_TYPE_MONO, &hRT )"
016E5EE0  "getDevice()->GetBackBuffer(0,0, D3DBACKBUFFER_TYPE_MONO, &hRT )"
016E5F20  "present_result"
016E5F48  "getDevice()->CreateAdditionalSwapChain(&d3dppm,&mDisplay[i].pSwapChain)"
016E5F90  "getDevice()->Reset(&d3dppm)"
016E6298  "Stereo"          <- read next to "VSYNC"/"FullScreen" at 0x00F42C27 (config lookup)
```

### 1.3 The Present site, with everything around it (MEASURED)

A whole-`.text` sweep for `call dword ptr [reg+disp32]` returns **zero** hits for every d3d9
vtable offset, so the receiver is always in a register. A sweep for the *vtable-reload* idiom
(`mov reg,[reg+disp]`, disp an exact device-vtable offset) over the whole image gives 43
candidates; exactly one is a real device call:

```asm
00F41D3E  mov  ecx,esi
00F41D40  call 0x00F41BC0                        ; flush helper
00F41D45  mov  eax,[esi+0x100]                   ; *the* IDirect3DDevice9
00F41D4B  mov  edx,[eax]
00F41D4D  push 0 / push 0 / push 0 / push 0
00F41D55  push eax                               ; arg1 = device
00F41D56  mov  eax,[edx+0x44]                    ; +0x44 = IDirect3DDevice9 slot 17 = Present
00F41D59  call eax
00F41D5B  cmp  eax,0x88760868                    ; D3DERR_DEVICELOST
00F41D60  jne  0x00F41D76
00F41D62  or   dword ptr [esi+0x448EE4],0x10
00F41D69  mov  ecx,ds:[0x186E65C]
00F41D6F  call 0x00F16A50                        ; device-lost handling
00F41D74  jmp  0x00F41D8E
00F41D76  cmp  eax,0x80004005                    ; E_FAIL
00F41D7B  je   0x00F41D8E
00F41D7D  mov  ecx,ds:[0x186E8BC]
00F41D83  push 0x16E5F20                          ; "present_result"
00F41D89  call 0x00F3AE50                         ; D3D error -> string/assert
```

then the engine presents **every additional display's own swap chain**:

```asm
00F41D8E  mov  ebp,1
00F41D93  cmp  dword ptr [esi+0x462694],ebp        ; <-- display count
00F41D99  jbe  0x00F41DC8
00F41D9B  lea  edi,[esi+0x46267C]                  ; <-- mDisplay[], stride 0x1C
00F41DA1  cmp  dword ptr [edi],0
00F41DA4  je   0x00F41DBC
00F41DA6  mov  edx,[edi-4]                         ; display size / rect
00F41DA9  mov  eax,[edi]                           ; pSwapChain
00F41DAB  mov  ecx,[eax]
00F41DB7  mov  eax,[ecx+0xC]                       ; ISwapChain9 slot 3 = Present
00F41DBA  call eax
00F41DBC  inc  ebp
00F41DBD  add  edi,0x1C
00F41DC0  cmp  ebp,[esi+0x462694]
00F41DC6  jb   0x00F41DA1
```

### 1.4 The frame-end function (MEASURED)

```
0x00F41C20 .. 0x00F41E66      (0x246 bytes, ends at int3 padding)
prologue bytes: 51 55 56 8B F1 57 8D 8E A0 00 00 00 E8 DF 4F 4E ...
   push ecx / push ebp / push esi / mov esi,ecx / push edi / lea ecx,[esi+0xA0] / call 0x01426C10
convention: __thiscall, ecx = sRender*, no stack args, no ret N
only caller: 0x00F41E7E, inside 0x00F41E70 (4 instructions, same convention)
```

Body: flush the pending work queues (`0x01426C10`), the framerate limiter (spin on
`ds:[0x150206C]` = `WaitForSingleObject`), `call 0x00F41BC0`, `Present`, the secondary swap
chains, then `SetEvent` (`ds:[0x1502168]`) to release the render thread.

### 1.5 The frame function (MEASURED)

```
0x00511D70 .. 0x00512455      (0x6E5 bytes)
prologue bytes: B8 40 11 00 00 E8 66 46 EE 00   ->  mov eax,0x1140 / call __alloca_probe
   then: push ebp / mov ebp,ecx        (ecx = the app object; ebp holds it because the
                                        0x1140-byte frame is addressed through esp)
   first act: call dword ptr ds:[0x1502470] = steam_api!SteamAPI_IsSteamRunning
   exits with: pop ebx / pop ebp / add esp,0x1140 / jmp eax   (a tail call)
```

Application fields are `[ebp+0x101xx]`. Verified call sites (MEASURED, `_work\sr_calls.py`):

| site | call | what |
|---|---|---|
| `0x00511DA5` | `0x00EFA1D0` | message pump / input |
| `0x00511E68` | `0x00511150` | per-frame init (reached again as `0x00511950` from the early-out path at `0x00511E39`) |
| `0x00511E8B` | `0x00510EA0` | |
| `0x00511EC3` / `0x00511A42` | `0x00504B10` (`jmp 0x00F10CD0`) | camera/display manager update, `ecx = [ebp+0x10168]` (both exit paths do it) |
| `0x00511F1A` / `0x00511A99` | `0x00F3BC90` | renderer pre-pass |
| `0x00512185` | `0x00FBB350` (`-> 0x00FBAF10`) | |
| `0x00512251` | `0x00511350` | |
| `0x005123E6` | `0x00EF8C90` | |
| `0x005123F1..0x00512428` | `call [sRender+0x18] / [+0x30] / [+0x28]` | the frame-end sequence on `[ebp+0x10148]` = the `sRender` object |
| **`0x00512430`** | **`0x00F41C20`** | **Present** |
| `0x0051243B` | `0x00FEF240` | `WaitForSingleObject` on a render-thread event |
| `0x00512448` | tail `jmp eax` | |

The frame is virtual: app vtable `0x0151F670`, `0x00511D70` = slot 6 (+0x18) (MEASURED,
`_work\sr_vptr.py`):

```
vtable 0x0151F670  slot 0 = 0x00515450 (dtor, sets the vtable then calls 0x00514F30)
                   slot 3 = 0x00512460   slot 4 = 0x00514F20   slot 5 = 0x00510620
                   slot 6 = 0x00511D70   <== THE FRAME FUNCTION
```

> **Corrected mis-reading, recorded so it is not repeated.** `0x00514F30` contains
> `mov edi,8 / mov ecx,esi / call 0x00511D70 / dec edi / jne` — an 8-iteration loop around the
> frame function. It looks like "8 frames per tick", but it is the **destructor body** (app
> vtable slot 0; `0x00515450` installs the vtable and then calls it, and its address has **zero**
> `call` sites in the image). It is cleanup, not a frame loop. **MEASURED.**

> **DEAD END (MEASURED absence).** No `call` site for `0x00511D70` exists, no
> `call dword ptr [reg+0x18]` occurs anywhere in `.text`, and no
> `mov reg,[reg2+0x18]; call reg` shape is followed by a call through that register. Whoever
> dispatches app slot 6 — and whoever drives the frame function's own caller — uses a shape this
> binary does not use elsewhere (a runtime-populated function-pointer table is the likely
> candidate). **Do not hunt the outer tick loop.** The frame function, the frame-end function and
> the Present site are known; a counter hook on any of them answers "once per frame?" empirically.

---

## 2. The per-frame render phase, and the display loop (`0x00F3EA30`)

### 2.1 Prologue and call context (MEASURED)

```
0x00F3EA30  prologue: 83 EC 10 55 57 8B F9 33 ED      (sub esp,0x10 / push ebp / push edi /
                                                       mov edi,ecx / xor ebp,ebp)
            __thiscall, ecx = sRender*
            ends: pop esi / pop ebx / pop edi / pop ebp / add esp,0x10 / ret
            vtable 0x016E5E18 slot 6 (also reached through 0x00534190 = a tail-jump into the ctor)
```
Frame order inside it (MEASURED from the listing):

```asm
00F3EA45  mov  ecx,ds:[0x1870884] / mov eax,[ecx] / mov edx,[eax+0x18] / call edx
00F3EA95..00F3EAE7  build the scene target and its rect (from [edi+0x120]/[edi+0x124]
                    = sRender::mScreenSize), several render-context calls (0x0109D7xx/0x010Axxxx)
00F3EAEC  mov  ecx,ds:[0x186E23C]        ; sBioCamera singleton
00F3EAF2  mov  eax,[edi+0x448EB8]        ; sRender->mpPrimaryScene
00F3EAFA  mov  edx,[edx+0x24]            ; sBioCamera slot 9 = the camera update
00F3EAFD  push ebp (0) / push eax        ; (scene, 0)
00F3EB00  call edx                       ; *** camera update for this pass ***
00F3EB18..00F3EB54  clamp/manage the draw buffers
00F3EB7C  call 0x010A0D00 / 0x00F3EBE6 call 0x00EFFA30 (ecx = sBioCamera)  <- the 8-slot view loop
00F3EC13  push esi / mov ecx,edi / call 0x00F3A700
00F3EC45..00F3EC6E  more render-context state (0x010B3870 = a draw setup)
00F3EC79  call 0x0109D870
00F3EC80  mov ecx,[edi+0x448EB8] / mov edx,[ecx+0x254] / call 0x010A68C0
00F3EC96  push 0 / push esi / mov ecx,edi / call 0x00F3A9E0     <- DRAW pass for display index 0
00F3EC9F  mov  ebx,1
00F3ECA4  cmp  dword ptr [edi+0x462694],ebx                     ; *** display count ***
00F3ECAA  jbe  0x00F3ED3E                                       ; only one display -> done
       ---- per additional display: ------------------------------------------------
00F3ECB9  call 0x0109D7F0        ; bind that display's target/layer
00F3ECBE  mov  ecx,ds:[0x186E23C]
00F3ECC4  mov  edx,[edi+0x448EB8]
00F3ECCC  mov  eax,[eax+0x24]                    ; the camera update again
00F3ECCF  push ebx                               ; *** ARG1 = THE DISPLAY INDEX ***
00F3ECD0  push edx / push esi
00F3ECD2  call eax                               ; *** per-display camera update ***
00F3ECD6  call 0x0109D870
00F3ECE1..00F3ED0D  bind the display's layer/depth and call 0x0109F800
00F3ED21  call 0x010A68C0
00F3ED28  push ebx / push esi / mov ecx,edi / call 0x00F3A9E0   ; *** DRAW pass for display ebx ***
00F3ED31  inc  ebx
00F3ED32  cmp  ebx,[edi+0x462694]
00F3ED38  jb   0x00F3ECB0
       -----------------------------------------------------------------------------
00F3ED3E  call 0x0109DDA0 / store the pass result into sRender+0x448E64/6C/74
```

**MEASURED: `0x00F3EA30` renders the whole scene once per display, and passes the display index
to the camera update and to the draw routine `0x00F3A9E0` (which selects the per-display entry
`[sRender + 0x4540B8 + 0x1C*i]`). The engine can therefore already render the scene more than
once per frame; only the display count stands in the way.**

### 2.2 Where the display count is set (MEASURED)

`[sRender+0x462694]` is **written exactly once** in the whole image, at `0x00F4301E`, in the
device/display initialisation path:

```asm
00F42FCF  push 0x26 / push 0x46                       ; D3DPRESENT_PARAMETERS flags
00F42FD1  mov eax,[esi+0x108]                         ; IDirect3D9*
00F42FD7  mov edx,[eax]
00F42FD9  mov edx,[edx+0x40]                          ; IDirect3D9 slot 16 = CreateDevice
00F42FEC  call edx
00F42FF3  call 0x00F3AE50                             ; check the HRESULT
00F42FF8  mov eax,[edi] / mov ecx,[eax] / mov edx,[ecx+0x10] / push eax / call edx
00F43007  mov [esi+0x4540B8],eax                      ; primary display object
00F4300D  mov eax,ds:[0x186E870]
00F43012  mov ecx,[eax+0x12C]
00F43018  mov [esi+0x46265C],ecx
00F4301E  mov dword ptr [esi+0x462694],ebp            ; ebp = 1  <-- *** displayCount = 1 ***
00F43024  mov eax,ds:[0x186E870]
00F43029  cmp dword ptr [eax+0x130],ebx               ; ebx = 0
00F4302F  je  0x00F43084
00F43031  mov dword ptr [esp+0x50],ebp                ; a SECOND ("slave"/secondary) display exists
00F43035  mov edx,[eax+0x130]
00F43047  mov ecx,[eax] / push 0x16E5FE8              ; a string for the secondary swap chain
00F4304E  lea edx,[esi+0x46267C] ; push edx           ; &mDisplay[1].pSwapChain
00F4305B  mov eax,[ecx+0x34] / call eax               ; IDirect3D9 slot 13 = CreateAdditionalSwapChain
```

and the matching creation loop at `0x00F422D4..0x00F4232B`:

```asm
00F422D4  cmp  dword ptr [esi+0x462694],edi           ; displayCount vs 1
00F422DA  jbe  0x00F4232F                             ; no extra display -> skip
00F422DC  lea  ebp,[esi+0x46267C]                     ; &mDisplay[1]
00F422FC  push 0x16E5F48                              ; "getDevice()->CreateAdditionalSwapChain(...)"
00F42305  mov  eax,[esi+0x100]                        ; the device
00F42314  mov  eax,[ecx+0x34]                         ; IDirect3DDevice9 slot 13
00F42317  call eax
00F42321  inc  edi / add ebp,0x1C / cmp edi,[esi+0x462694] / jb 0x00F422E2
```

```asm
00F422B1  call dword ptr ds:[0x15022F4]               ; the import used to create the display window
00F3BDFD..00F3BE35   on device loss: Present (ISwapChain9 slot 3 = [ecx+0xC]) every mDisplay[i]
```

**MEASURED layout:** `mDisplay[]` at `sRender+0x46267C`, stride **0x1C**, entry = `{ +0x00 u32 ?,
+0x04 pSwapChain }`; `sRender+0x462694` = the display count; `sRender+0x4540B8 + 0x1C*i` = the
per-display render object used by the draw pass; `sRender+0x48E4C`/`0x448E64`/`0x448E6C`/`0x448E74`
= the per-pass result slots.

**MEASURED:** the string `"Stereo"` is read by the config system at `0x00F42C27`, right beside
`"VSYNC"` (`0x00F42CF3`) and `"FullScreen"` (`0x00F42C42`), and stored into `sRender+0x448EE0`
(`mov byte ptr [esi+0x448EE0],al` at `0x00F42C36`). `"Stereo"` is a `D3DPRESENT_PARAMETERS` /
`D3DCREATE_STEREO`-era setting; **INFERRED:** the engine has a stereo presentation path, but it is
a driver-level "one swap chain, both eyes interleaved" mode — it does **not** give the mod two
separately capturable pictures, so it is not the mechanism the mod wants.

### 2.3 The per-view loop (`0x00EFFA30`) — what it really does (MEASURED)

It is called once per pass from `0x00F3EBE6` with `ecx = sBioCamera`, arg1 = a 4-dword rect
(from `sRender->mScreenSize`), arg2 = the render context (`sRender+0x194`), `ret 8`.

```
prologue: 55 8B EC 83 E4 F0 81 EC 34 04 00 00 53 56 8B F1
```

```asm
00EFFAB7  add  esi,0x50                  ; esi = sBioCamera+0x50
00EFFABA  mov  dword ptr [esp+0x10],8    ; EIGHT slots
00EFFAC2  cmp  byte ptr [esi-0x10],0     ; slot record +0x10 = ACTIVE flag
00EFFAC6  je   0x00EFFB2A                ;   -> esi += 0x190 (skip, same edi)
00EFFAC8  cmp  dword ptr [esi-0x14],0    ; slot record +0x0C
00EFFACC  jne  0x00EFFB2A                ;   -> skip
00EFFACE  xor  edi,edi
00EFFAD0  test ecx,ecx                   ; ecx = a count that arrived in the register
00EFFAD2  je   0x00EFFB18
       --- inner loop, at most `ecx` pieces: split the bounding box by one rect ------
00EFFAD4  mov  [esp+0x1C],ecx
00EFFAD8  mov  edx,[esi-4] / mov ecx,[esi-8] / mov eax,[esi] / mov ecx,[esi+4]
          -> {[esi-8],[esi-4],[esi],[esi+4]} stored as a 4-dword rect on the stack
00EFFAE4  mov  edx,edi / shl edx,4 / add edx,[esp+0x18]   ; output slot = base + 16*edi
00EFFAF8  push edx ; lea eax,[esp+0x24] ; push eax ; mov ecx,ebx ; call 0x00EF0200
00EFFB09  add  edi,eax                   ; eax = 0 or 1 (a returned rectangle)
00EFFB0B  add  ebx,0x10
00EFFB0E  dec  dword ptr [esp+0x1C] / jne 0xEFFAD8
00EFFB14  mov  ebx,[esp+0x14]            ; restore the output base
00EFFB18  swap [esp+0x14] / [esp+0x18]   ; hand the produced rects to the other buffer
00EFFB2A  add  esi,0x190
00EFFB30  dec  dword ptr [esp+0x10] / jne 0xEFFAC2
       --- then: clear/scissor setup for whatever was collected ----------------------
00EFFB36  test ecx,ecx / je 0xEFFC06
00EFFB4C  (per collected rect) test width/height != 0
00EFFB6A  mov  eax,[sBioCamera+0xCD0]    ; mScreenPlNo (DTI type 0x21000A), 4 bytes
00EFFB70..00EFFBC8  unpack those 4 bytes as 4 floats * ds:[0x1528A9C]  -> a clear colour
00EFFBD1  call 0x010A0130                ; render context: set viewport
00EFFBEF  call 0x010A2A40                ; render context: push a draw command
```

and `0x00EF0200` is a **pure 2D rectangle clipper** (MEASURED: 0xEFFA30's `ecx` argument is the
loop bound; every path through `0x00EF0200` ends in `ret 8` returning 0 or 1, and the function
contains **no calls at all** — `_work\sr_calls.py` finds zero call sites in `0xEF0200..0xEF0645`;
it reads `[ecx]`,`[ecx+4]`,`[ecx+8]`,`[ecx+0xC]` and writes up to four 16-byte rectangles).

> **Corrected earlier reading:** this loop is **not** "draw 8 cameras". It walks the 8
> `sBioCamera+0x30+0x190k` records and, for each active record, **clips the render region by that
> record's rectangle**. It sets up *where* views go; the visible draw is the `0x00F3A9E0` pass in
> §2.1.

**MEASURED: the slot records the renderer walks are the same ones the camera code walks** — base
`sBioCamera+0x30`, stride `0x190`, 8 entries. The record layout the renderer itself proves:

| offset | meaning | evidence |
|---|---|---|
| `+0x00` | non-null pointer (registration also checks it, `0x4F9200`) | `0x503B34 mov edi,[ebx]` |
| `+0x04` | the camera object | `0x4DA59A mov [ecx+esi+0x34],edx` |
| `+0x0C` | must be 0 for the view loop to consider the record | `0x00EFFAC8` |
| `+0x10` | ACTIVE byte; 0 => the record is skipped | `0x00EFFAC2` |

and the rectangle the renderer clips by is `[record+0x08]`, `[record+0x0C]`, `[record+0x10]`,
`[record+0x14]` (MEASURED: `[esi-8]`, `[esi-4]`, `[esi]`, `[esi+4]` with `esi =
sBioCamera+0x50+0x190k`; the record's own `+0x0C` is a dword that must read 0, and it is also
the third rectangle member, so the field is *both* the "enabled" test and the rect's y — i.e. the
renderer only accepts a rect whose y is 0 and derives an x/width/height from `+0x08/+0x10/+0x14`).
**INFERRED:** that is a conservative read of an ambiguous aliasing; the load-bearing part is
"active byte at `+0x10`, dword `+0x0C` must be zero", which is quoted directly.

In the project's own last runtime dump **only slot 0 was populated** (`1 plausible camera(s) in
the slot table`, `cam = 22088360`, vtable `0152D620` = `uCameraCtrl`), so one view region is
produced today.

### 2.4 The negative result that matters (MEASURED)

* A displacement scan over every decoded instruction of the whole image for offsets
  `0x12A0..0x161F` (the `mViewportCamera` matrix block) finds only unrelated fields of other
  classes (`0x1350`, `0x1568`, `0x1570`, `0x1609`, `0x1610/0x1614/0x1618` = `CameraPatch_*`).
  **No reader of the `mViewportCamera` matrix block exists.** (`_work\sr_vpcam.py`.)
* The renderer's own code (`0xEFFA30..0xF00000`, decoded straight from the image because the
  analysis cache has holes there) contains no displacement into that range; the three
  `+0x13BC`/`+0x13C0` hits are offsets of the render command-buffer object (`esi` is a scratch
  buffer there, not `sBioCamera`). **MEASURED.**

**INFERRED (strong):** the picture is produced from the slot cameras
(`[sBioCamera+0x34+0x190k]`) and their pose blocks, **not** from `mViewportCamera`.
`mViewportCamera` is written every frame by `0x00503880` (`0x00504773`-`0x00504902`) and is a
fine place to *observe* per-view matrices, but nothing renders from it. Do not build the stereo
design on reading or writing it.

---

## 3. The pose → eye path: there is no hidden eye offset (MEASURED)

```asm
005F80B0  push esi                        ; uCamera-family slot 18, thiscall, ret 4
005F80B1  mov  esi,[esp+8]                ; out Matrix*
005F80B5  lea  eax,[ecx+0x60]             ; up
005F80B8  lea  edx,[ecx+0x70]             ; target
005F80BB  push eax / push edx
005F80BC  add  ecx,0x50                   ; pos   <-- the eye, verbatim
005F80C0  push ecx
005F80C1  mov  ecx,esi
005F80C3  call 0x00E6FD20                 ; MakeViewMatrix(eye,target,up,out)
005F80CB  ret  4
```

`0x00E6FD20` is a *pure* look-at. It reads only the three pointers (`[ebp+8]`, `[ebp+0xC]`,
`[ebp+0x10]`), computes `forward = target - eye`, tests it against the epsilon `ds:[0x15110A0]`,
normalises by `ds:[0x1510398]` (1.0f), then `right = up × forward`, re-orthogonalises `up` and
writes the 4x4. **MEASURED: no constant offset, no head/eye field, no per-eye term anywhere in
its 163 instructions.**

Other pose copies that exist but are **not** the render eye (MEASURED; all fed from the same
values and none read back by the view builder):

* `[cam+0xD0]`, `[cam+0xE0]`, `[cam+0xF0]` — the published `Vector4` pose written by `0x005FBE00`;
  `+0x100` = fov.
* `[cam+0x4AB0]`, `[cam+0x4AC0]` — the controller's own copies of pos/target, made in `0x0060C9F0`.
* `mCameraOrg[8]` (`+0xE30`) and the shadow copy at `+0x1030` — **dead for rendering**: the only
  reader of `mCameraOrg` in the whole image is `0x004FCB70`, which copies it into `+0x1030`.

**Consequence: the mod owns the eye completely — `[cam+0x50]` (and `[cam+0x70]`) *is* the eye.
The corollary is that any eye translation must be applied per view-matrix build, because the
engine builds the same camera's matrix several times per frame** (this is the same fact
`cam_steer.cpp` recorded as "the engine hands the same camera's vectors to `MakeViewMatrix`
several times per frame").

Live camera class note (MEASURED): the singleton's vptr is `0x0151A394` = `sBioCamera`'s static
vtable `0x0151A380` + 0x14, i.e. the live object is a *derived* class whose ctor `0x00503460`
installs that vptr (`mov dword ptr [edi],0x151A394` at `0x00503471`); its slots 0..11 correspond
to `sBioCamera`'s slots 5..16, which is why slot 11 = `0x00503880` = `sBioCamera::Update`
(`_work\sr_vptr.py`).

---

## 4. What a per-eye pose needs, and when

The pose is consumed inside the camera update (`0x00503880`'s per-slot loop calls
`[cam+0x48]` = `0x5F80B0` at `0x00503B4A`), and the render phase runs **after** it
(`0x00F3EB00` camera update → `0x00F3EBE6` view loop → `0x00F3ECD2` per-display camera update →
`0x00F3ED2C` draw). **MEASURED ordering.** Therefore:

* A frame-level write from the `Present` hook is erased before the view is built — exactly what
  `cam_steer.cpp`'s comments already record.
* The last code that sees the pose before it becomes a matrix is `0x005F80B0`, which is where the
  per-eye translation must be applied. The existing detour already has `ecx` = the camera object.
* For **Seam A** (one camera per display), the per-display camera update (`0x00F3ECD2`) rums
  again for each display index — so the same `0x5F80B0` detour sees an extra build per pass, and
  it can select the eye from "which pass am I in", counted at the loop head `0x00F3ECB0` /
  `0x00F3ECCF`.
* For **Seam B** (two slots in one pass), the detour can select the eye from the camera object
  pointer (`ecx`), which is unique per slot.

---

## 5. The two concrete mechanisms

### Seam A — the display loop (whole scene rendered once per display)

**What the engine needs (MEASURED):**

| field | role |
|---|---|
| `sRender+0x462694` | display count; the write site is `0x00F4301E` (`= 1`) |
| `sRender+0x46267C + 0x1C*i` | `mDisplay[i]`, `+0x00` = size/rect dword, `+0x04` = `pSwapChain` |
| `sRender+0x4540B8 + 0x1C*i` | the per-display render object the draw pass uses (`0x00F3AA13`) |
| `sRender+0x448E64/6C/74` | the per-pass result slots |

**Cost/risk:** raising the count makes the engine call
`IDirect3D9::CreateAdditionalSwapChain` (`0x00F42317`) and then present that swap chain every
frame (`0x00F41DBA`) — i.e. a second real window/back buffer appears, and the game's rendering is
duplicated into it. For a VR mod that wants to *capture* two pictures, a second swap chain is
awkward (both are composited by DWM), so this seam is best used as **evidence** ("the engine can
render twice per frame") rather than as the shipping mechanism — unless the mod creates the
second swap chain on an offscreen swap chain it owns, which the engine will happily render into.

### Seam B — two view regions in one scene pass

Populate a second record in `sBioCamera+0x30+0x190k`: `+0x00` non-null, `+0x04` a second camera
object, `+0x0C = 0`, `+0x10 = 1`, and give that record (or its camera) the right-hand rectangle.
`0x00EFFA30` then produces the second region and the render context draws into it.

**Risks (MEASURED readers of the same slot table that a second record would disturb):**
`0x00503880`'s own per-slot processing (`0x004FE9F0`, `0x00502F60`, `0x005032D0`), the frustum
build `0x005F8180`, `0x0054D730` (the engine's own "distance from the camera" query, which reads
`[slotcam+0x50]`), `0x4F9200` (k-th active slot) and `0x4F8F50` (`SlotOfPlayer`, iterating
`[this+0xCF0+4*i]`, i<2). A second record therefore also adds a second *game* view, which is
exactly what split-screen is — fine as an experiment, not as the final stereo path.

### Recommended shape for the mod

Given the above, the smallest reliable design is:

1. **Do not try to make the engine render twice per frame.** Instead make the single scene pass
   produce **two pictures**, by rendering the scene from the left-eye pose into the left
   rectangle and from the right-eye pose into the right rectangle — i.e. Seam B with two regions,
   or (if a second record turns out to disturb gameplay) **two calls of the render phase with the
   camera pose swapped in between**:
   * hook `0x00F3EA30` (prologue `83 EC 10 55 57 8B F9 33 ED`, `__thiscall`, `ecx = sRender`),
     let it run once with the left-eye pose, then set the right-eye pose and call it again;
   * `0x00F3EA30` is safe to call from the frame thread — it is a normal method, and every
     hazard below is about *where* it is called from, not about the call itself.
2. **Never call it from the `Present` detour.** `Present` is issued at `0x00F41D59` *inside*
   `0x00F41C20`; a d3d9 Present is already in flight and the back buffer is being presented.
   The project's own `ERR09`/"no D3D9 texture creation inside a frame" note is the same class of
   failure. Call it **before** the frame reaches `0x00F41C20` — from a detour on `0x00F3EA30`, or
   from `0x00F3BC90`.
3. **Keep both passes on the frame's thread.** The frame function pumps messages and calls
   `Present`, so it is the game/render thread; the proxy's compositor/readback runs on its own
   threads (`openxr_bridge.cpp`: "the game calls EndScene and Present from different threads").
   An engine call must come from the frame thread.

---

## 6. Exactly what a MinHook probe should write

**Probe 1 — count the frame (read-only, no risk).** Hook `0x00F41C20`
(`51 55 56 8B F1 57 8D 8E A0 00 00 00`, `__thiscall`; ≥5 relocatable bytes). Log a counter and
the return address `[esp]` (it will be `0x00512435`). This proves "the engine runs the frame-end
function once per displayed frame" and gives the frame id the rest of the probe keys on. The
frame function has an early-out at `0x00511E39` → `0x00511950` that skips the render, so count
both `0x00F41C20` and (for example) `0x00F3EA30` to see them diverge during loading.

**Probe 2 — per-eye pose, in the existing `0x005F80B0` detour.** The detour already has `ecx` =
the camera object:

```
right[]  = normalise(cross(up, target - pos));         // up = [ecx+0x60], target = [ecx+0x70],
                                                       // pos = [ecx+0x50]
float s  = (leftEye ? -0.5f : +0.5f) * ipd_metres;
pos[i]    += right[i] * s;                             // write [ecx+0x50]
target[i] += right[i] * s;                             // write [ecx+0x70]
```

Rules the project has already paid for, which apply verbatim:

* write **only** `[ecx+0x50]` / `[ecx+0x70]`; `+0x40/0x44/0x4C` are far/near/fov, and
  `mCameraOrg`/`+0x1030` are dead;
* re-derive the offset from the engine's *current* value every call (anchor, never accumulate) —
  the same camera's vectors reach the builder several times per frame;
* filter by camera object identity: only steer the camera the slot table actually renders from
  (`[[0x186E23C]+0x34+0x190k+0x04]`; the last dump had exactly one, slot 0, vtable `0x0152D620`).
  Without the filter the quake/blur/reflection cameras (`uCameraQuake`, `uCameraBlur`, … — 8
  classes share slot 18) move too, and the picture multiplies instead of gaining parallax.

**Probe 3 — per-eye projection.** `[ecx+0x4C]` (fov) is on the same object. The project has not
settled whether it is vertical or horizontal fov, and a plain fov field cannot express the
asymmetric per-eye frustum. Either write `[ecx+0x4C]` per eye (cheap, symmetric, no convergence)
or fix the projection matrix after it is built and before upload (the matrix probe already
identifies the `PROJ` register per draw — README Stage 2).

**Probe 4 — the second view.** Populate the second slot record (§5, Seam B) or call `0x00F3EA30`
a second time (§5, recommended shape), and log the engine's own evidence: the 8 active flags at
`sBioCamera+0x30+0x190k+0x10`, the display loop index at `0x00F3ECB0`, and the display count at
`sRender+0x462694`. Never trust a write that is not read back.

---

## 7. Dead ends — with the evidence that killed each one

1. **`call dword ptr [reg+disp]` to find the Present call.** MEASURED: the `FF /2 mod=10` form
   with disp = 0x44, 0x48, 0xA4, 0xA8 or 0xAC occurs **0 times** in the whole `.text` (sweep over
   `0x401000..0x1501200`). RE6 always reloads the slot: `mov eax,[edx+0x44]; call eax`. The same
   trap the project already recorded for `call [reg+0x48]`.
2. **`re6dis.py xref <VA>` for code references.** MEASURED: it returns nothing for the d3d9 error
   strings (`0x016E5E90`, `0x016E5F20`, `0x016E5F48`, `0x016E5F90`) — the engine reaches them with
   `push imm32`/`lea`, and the recursive descent never covered big parts of the renderer anyway.
   Use a raw little-endian dword search over `.text` **plus alignment validation**
   (`_work\sr_strrefs.py`).
3. **Scanning `an.decoded`, or trusting the cache's coverage.** MEASURED: `load_analysis()` leaves
   `Analysis.decoded` **empty** (instructions decode lazily from `blocks`), so a scan over it
   silently reports 0 hits for everything; and `0x00F42D6A`/`0x00F42DC4` are **not inside any
   cached function at all**, even though the `0x00F01000` window reports >100% coverage (cached
   function ranges overlap, so the coverage number lies upward). Renderer questions must be
   answered by decoding the image (`_work\sr_dis.py`).
4. **`0x00514F30` as an "8 frames per tick" loop.** It is the app destructor's body (vtable slot
   0, run from `0x00515450`); MEASURED: its address has zero `call` sites.
5. **`mViewportCamera` as the renderer's view source.** MEASURED: nothing reads a displacement in
   `0x12A0..0x161F` as a matrix; the renderer's own code confirms it. `0x00503880` writes 8
   matrices per frame and no consumer exists.
6. **The 8-slot loop in `0x00EFFA30` as "8 cameras drawn".** MEASURED: it clips the render
   region per slot (`0x00EF0200` is a rectangle clipper with **no calls** in its body) and pushes
   viewport/draw commands; the visible pass is `0x00F3A9E0`, called per display from `0x00F3ED2C`.
7. **`call dword ptr [reg+0x18]` for the app's frame slot.** MEASURED: 0 occurrences, and no
   `mov reg,[reg2+0x18]; call reg` shape is followed by a call through that register. The
   dispatcher of app slot 6 uses a shape this binary does not otherwise use — do not chase it.
8. **Expecting an eye-offset field.** MEASURED: `0x00E6FD20` is a pure look-at over its three
   pointer arguments and `0x005F80B0` passes `[ecx+0x50]` verbatim. The mod supplies the whole
   translation.
9. **Hooking `0x00E6FD20` alone to steer.** It has 71 callers (actors, effects, shadows,
   cameras); the pose written there applies to whatever is being built. Use `0x005F80B0` and
   filter by camera identity.

---

## 8. Offset / VA cheat sheet (all MEASURED unless marked)

```
sRender singleton            0x0186E8BC   vtable 0x016E5E18, ctor 0x00F3F0B0 = slot 3
  +0x100  IDirect3DDevice9*        (0x00F41D45, 0x00F4219A, 0x00F42305)
  +0x108  IDirect3D9*              (0x00F42D6F, 0x00F42781)
  +0x120/+0x124 mScreenSize (w,h)  (registered, DTI type 0x30011)
  +0x194  the render context used by the view loop
  +0x448EB8 mpPrimaryScene
  +0x448E90/94 pending work flags      +0x448EE4 device-state bits (0x10 = lost)
  +0x448EE0 the "Stereo" config byte (written at 0x00F42C36)
  +0x46267C mDisplay[] (stride 0x1C; +0x04 = pSwapChain)
  +0x462694 DISPLAY COUNT (written once, 0x00F4301E = 1)
  +0x4540B8 + 0x1C*i  the per-display render object used by the draw pass
  +0x48E4C / 0x448E64 / 0x448E6C / 0x448E74  per-pass result slots
  frame phase : 0x00F3EA30  (per-display loop 0x00F3ECB0..0x00F3ED38; draw 0x00F3A9E0)
  pre-pass    : 0x00F3BC90
  frame end   : 0x00F41C20 -> 0x00F41D59 IDirect3DDevice9::Present
app object     vtable 0x0151F670, slot 3 = 0x00512460, slot 5 = 0x00510620,
               slot 6 (+0x18) = 0x00511D70 = THE FRAME FUNCTION
  frame function 0x00511D70..0x00512455, prologue B8 40 11 00 00 E8 .. (alloca 0x1140)
  +0x10148 the sRender object   +0x10168 the camera/display manager
sBioCamera singleton          ds:[0x0186E23C]
  live vptr 0x0151A394 = 0x151A380 + 0x14 (derived class, ctor 0x00503460)
  +0x0030 slot records, stride 0x190, 8 of them:
        +0x00 pointer (non-null)   +0x04 camera object   +0x0C must be 0   +0x10 ACTIVE byte
        +0x08/+0x10/+0x14 = the view rectangle the renderer clips against (see §2.3)
  +0x0CE0 mScreenType  +0x0CF0 mScreenPlNo  +0x0D24/28/2C/30 viewport x,y,w,h
  +0x0D80 mCameraParam +0x0DE4 mPartnerCamFlg
  +0x0E30 mCameraOrg[8] (stride 0x40)  — DEAD for rendering
  +0x1030 second pose array[8]         — DEAD for rendering
  +0x12A0 mViewportCamera[8] (stride 0x60): +0x04 valid byte, +0x10 16-float matrix,
        +0x50 fov — written every frame by 0x00503880, READ BY NOTHING
  +0x15A0 mDispCtrlFlag +0x15B0 mWipe +0x1610/14/18 CameraPatch_*
  camera object (uCameraCtrl, vtable 0x0152D620):
        +0x0040 far  +0x0044 near  +0x004C fov
        +0x0050 cameraPos  +0x0060 cameraUp  +0x0070 targetPos     <- THE EYE
        +0x00D0/0xE0/0xF0 published pose, +0x0100 fov
renderer view loop            0x00EFFA30  __thiscall ecx=sBioCamera, 2 stack args, ret 8
                              called once per pass from 0x00F3EBE6
pose -> view matrix           0x005F80B0 -> 0x00E6FD20 (pure look-at, no eye offset)
```

---

## 9. What is still unverified (one runtime experiment each)

1. **Does the frame function run exactly once per displayed frame?** Probe 1 counts it. The
   early-out at `0x00511E39` skips the render, so it may not be 1:1 during loading.
2. **Which thread owns `0x00F3EA30` / `0x00F41C20`.** Both are on the frame path, but the proxy's
   readback runs elsewhere; log `GetCurrentThreadId()` in the `Present` detour and in a
   `0x00F3EA30` detour.
3. **Whether a second display (Seam A) is actually reachable from the mod.** The engine's own
   path needs `[0x186E870+0x130]` (a secondary-window handle) to be non-zero, which comes from
   the game's display configuration; forcing `displayCount` without that leaves
   `mDisplay[i].pSwapChain` null (the creation loop is guarded by the same count, so it would try
   to create one, but the window/rect comes from `[0x186E870+0x12C/0x130]`).
4. **What `[sBioCamera+0xCD0]`'s four bytes are** (read as a clear colour at `0x00EFFB6A`,
   registered as `mScreenPlNo`, DTI type 0x21000A). With two view regions the mod may need to
   leave it alone or set it deliberately.

---

## 10. Scratch tooling produced by this investigation (`_work\`, all UTF-8)

| file | purpose |
|---|---|
| `sr_lib.py` | cached whole-image instruction dump (`_sr_all_ins.pkl`); iterates `blocks`, never `decoded` |
| `sr_dis.py` | decode an arbitrary VA range straight from the image (the cache has holes over the renderer) |
| `sr_fn.py` | function boundaries around a VA + every `call` landing inside it |
| `sr_calls.py` | call graph over a range, annotated with known vtable slots |
| `sr_dump.py` | filtered listing over a range (e.g. only `call` lines) |
| `sr_vptr.py`, `sr_vtables.py` | slot -> function tables read from a live vptr / a guessed vtable base |
| `sr_rawdword.py` | raw dword references to a VA (the only xref that works for `lea`/`push imm` targets) |
| `sr_strrefs.py` | aligned search for the code that uses a given string literal |
| `sr_comcall.py`, `sr_sweep.py`, `sr_slot.py`, `sr_slot2.py` | sweeps for indirect/COM call shapes |
| `sr_vpcam.py`, `sr_stride.py` | readers of the `mViewportCamera` region and 0x60-stride walkers |
| `sr_imports.py`, `sr_iat_refs.py`, `sr_thunks.py`, `sr_importcalls.py` | import table, `ff 25` thunks, every IAT call site |
| `sr_global.py`, `sr_vcall_global.py` | uses of the render/camera singletons and virtual calls on them |
| `sr_dispcount.py` | the display-count writer and the `mDisplay[]` users |
| `sr_coverage.py` | per-megabyte coverage of the analysis cache (proved the renderer holes) |
