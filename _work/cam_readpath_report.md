# RE6 camera read path — offline static report (2026-09-25)

Image: `C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6\BH6.exe` (ImageBase `0x400000`, 32-bit DX9,
MT Framework). All work is static over the existing analysis cache (`_work\bh6_analysis.json`,
1.87M instructions / 3722+ functions) via `scripts\re6dis.py` and `scripts\disasm_lib`.

Every claim below is tagged:

* **MEASURED** — read out of the binary (bytes/listing quoted, or a count over the whole image).
* **INFERRED** — follows from the measured facts but not itself observed.
* **ASSUMED** — plausible, unverified; listed so it can be tested rather than trusted.

---

## 0. The answer, in the form the C++ hook needs

> Hook **VA 0x005F80B0** (RVA **0x1F80B0**), 13 instructions, prologue
> `56 8B 74 24 08 8D 41 60 8D 51 70 50 83 C1 50 52 51 8B CE E8 … C2 04 00`.
> **thiscall**: `ecx` = the camera object (a `uCameraCtrl`, or any `uCamera`-family instance),
> one stack argument at `[esp+4]` = `Matrix* out`.
> **The view source is `[ecx+0x50]` cameraPos, `[ecx+0x60]` cameraUp, `[ecx+0x70]` targetPos**
> (plus `[ecx+0x4C]` fov, `[ecx+0x44]` near, `[ecx+0x40]` far).
> This function calls the engine's look-at builder `0x00E6FD20` once per camera per frame; its
> caller is `sBioCamera::Update` (`0x00503880`, vtable slot 11), which walks up to 8 camera slots
> and stores each resulting matrix into `mViewportCamera[k]` = `sBioCamera + 0x12A0 + 0x60*k`
> (matrix at `+0x10` of each entry).
> Head tracking = rewrite `[ecx+0x70]` (and optionally `[ecx+0x60]`), then run the trampoline.

If only one hook can be installed, install this one. It is the *last* code that reads the pose
before it becomes the frame's view matrix, it is shared by the whole `uCamera` family, and it
cannot be "overwritten later in the frame" the way every parameter array can.

---

## 1. **Correction (MEASURED): 0x4FF9B0 is *not* the mCameraOrg writer — it is a reset**

`scripts\re6dis.py xref 0xFF9B0` →
```
references to 0x004FF9B0
  dword at 0151A3A8 (.rdata)
1 reference(s); 1873873 instructions decoded to look for them
```
`0x151A3A8 = 0x151A380 + 0x28` = **vtable slot 10 of `sBioCamera` (vtable 0x151A380)**. There is no
call site anywhere: the function is reached only virtually.

Its body (VA `0x4FF9B0..0x50055F`, 463 instructions) is one unrolled initialiser:
```
004FF9B0  83EC10              sub  esp,0x10
004FF9B3  53 56 57            push ebx / esi / edi
004FF9B6  8BF1                mov  esi,ecx                     ; this = sBioCamera
004FF9B8  E8B3BD9F00          call 0x00EFB770
004FF9C1  899E240D0000        mov  [esi+0xD24],ebx             ; viewport rect
004FF9CD  C7862C0D000000050000  mov [esi+0xD2C],0x500           ; 1280
004FF9D7  C786300D0000D0020000  mov [esi+0xD30],0x2D0           ; 720
004FF9E1  E85A98FFFF          call 0x004F9240
004FF9E8  E8B399FFFF          call 0x004F93A0
004FF9ED  E84ECFFFFF          call 0x004FC940
004FF9F5  F30F100D64115401    movss xmm1,ds:[0x1541164]        ; constants, from .data
004FF9FD  F30F102DD4A95101    movss xmm5,ds:[0x151A9D4]
...
004FFA71  F30F118E34100000    movss [esi+0x1034],xmm1          ; writes the +0x1030 group
004FFA81  F30F118630100000    movss [esi+0x1030],xmm0          ; 0
004FFA91  F30F1186300E0000    movss [esi+0xE30],xmm0           ; and the +0xE30 group
004FFA99  D98634100000        fld   dword ptr [esi+0x1034]     ; <- reads back what it JUST wrote
004FFA9F  D99E340E0000        fstp  dword ptr [esi+0xE34]
```
The "parallel source group" that the earlier `check_copy.py` heuristic "found" is an artefact:
MSVC stores the constant into the `+0x1030` array with `movss`, then re-loads it with `fld` and
stores it to the `+0xE30` array with `fstp`. **Both arrays receive the same constants; nothing is
copied from one to the other.** (Same pattern at +0x1040→+0xE40, +0x1050→+0xE50, +0x1070→+0xE70 …)

The reset also does, at its end:
```
0050049E  A1BCE88601          mov  eax,ds:[0x186E8BC]          ; app/display singleton
005004A3  8B9020010000        mov  edx,[eax+0x120]             ; screen width
005004A9  8BB824010000        mov  edi,[eax+0x124]             ; screen height
...
00500519  898E240D0000        mov  [esi+0xD24],ecx             ; viewport x,y,w,h
0050051F  8986280D0000        mov  [esi+0xD28],eax
00500527  89962C0D0000        mov  [esi+0xD2C],edx
0050052F  89BE300D0000        mov  [esi+0xD30],edi
```
i.e. "reset all camera poses to defaults, then recompute the viewport rect for the current screen
size". That is why it runs exactly twice a session (process start + level build).

**Consequences (MEASURED/INFERRED)**

* `src\cam_hook.cpp` is hooking a *reset*. Its `camwrite` sweep writes to `+0xE40`, `+0x1050`,
  `+0x40`, and to "the object at `[hooked+0x1030]`", none of which is the render source — see §2.
* The **MEASURED** "no effect" result is now fully explained, not mysterious.
* The hook logs **before** the trampoline (`camera_write_detour` = `pushfd/pushad; …; jmp
  g_trampoline`) but prints "AFTER the write". So the "real pose at level load" it reported was the
  pose the *normal per-frame path* had already left in the object; the reset then wipes it. The log
  wording should be fixed, because it makes the reset look like a producer of poses.
* There is no `[hooked+0x1030]` *pointer* to a source object. `+0x1030` is float data (a second pose
  array). The sweep target "source object +0x50 / +0x60" was written into the middle of that float
  array.

---

## 2. **MEASURED: the two 8×0x40 pose arrays, and who reads them**

Hooked-up property table (`props sBioCamera`, dti 0x017C3164, size 0x1620, ctor 0x4FA570):

```
mCameraOrg[i]  = +0xE30 + 0x40*i   (i = 0..7)
    +0x00 cameraPos  +0x10 targetPos  +0x20 cameraUp  +0x30 fov  +0x34 nearPlane  +0x38 farPlane
```
A second array with the identical layout sits at `+0x1030 + 0x40*i` (0x1030..0x1228). It is *not*
in the property table. Between `mViewportCamera +0x12A0` (registered, type `0x200001`) and
`mDispCtrlFlag +0x15A0` there are exactly `0x300 = 8 * 0x60` bytes: an 8-entry per-slot camera
array (§4).

Full-image scan (`_work\scan_readwrite.py`, corrected so that a one-operand memory operand counts
as a *read* — `fld [mem]`, `push [mem]` — which the first version of that scan got wrong):

| base | functions that touch it | what they are |
|---|---|---|
| `+0xE30` (mCameraOrg) read | **1**: `0x004FCB70` | copies all 8 entries into `+0x1030` |
| `+0xE30` written | `0x004FF9B0` (reset, constants) and `0x004F9950` (push, below) | |
| `+0x1030` written | `0x004FCB70` (from mCameraOrg), `0x004FF9B0` (defaults) | |
| `+0x1030` read | **1**: `0x004F9A30` | pulls entry `i` back into a camera object |

`0x004FCB70` in full (218 instructions, the entire body):
```
004FCB70  D981300E0000      fld  dword ptr [ecx+0xE30]      ; mCameraOrg[0].cameraPos.x
004FCB79  D99930100000      fstp dword ptr [ecx+0x1030]     ; -> +0x1030 group
004FCB7F  D981340E0000      fld  dword ptr [ecx+0xE34]
...
004FCFEF  (entry 7) ...     96 stores in total, all 8 entries
```
**~ Nothing in the image reads `mCameraOrg` except this copy** (the only other hits are the
constructor's `lea ebx+0xE30` registration sites and displacement collisions in unrelated classes
whose own fields land on the same offsets). **INFERRED (strong): the renderer never reads
`mCameraOrg`.** That is the standing explanation for "writing `[this+0xE40]` every frame changes
nothing".

`0x004F9950` = `sBioCamera::PushOrg(Camera* src, int index)`, thiscall, `ret 8`:
```
004F9950  53                push ebx
004F9951  8B5C240C          mov  ebx,[esp+0xC]        ; arg2 = index
004F9955  57                push edi
004F9956  8BF9              mov  edi,ecx             ; this = sBioCamera
004F9958  83FB08            cmp  ebx,8               ; bounds check 0..7
004F9962  8B742410          mov  esi,[esp+0x10]      ; arg1 = src camera object
004F9987  D94650            fld  dword ptr [esi+0x50]        ; src+0x50  -> cameraPos
004F998D  8BC3              mov  eax,ebx
004F998F  C1E006            shl  eax,6                       ; index * 0x40
004F9992  D99C38300E0000    fstp dword ptr [eax+edi+0xE30]
004F99BD  D94670            fld  dword ptr [esi+0x70]        ; src+0x70  -> targetPos
004F99C2  (lea ecx,[ebx+0x39]; shl ecx,6 = 0xE40 + 0x40*i)
004F99D3  D94660            fld  dword ptr [esi+0x60]        ; src+0x60  -> cameraUp
004F99F6  D9464C            fld  dword ptr [esi+0x4C]        ; src+0x4C  -> fov   [+0xE60]
004F99FF  D94644            fld  dword ptr [esi+0x44]        ; src+0x44  -> near  [+0xE64]
004F9A08  D94640            fld  dword ptr [esi+0x40]        ; src+0x40  -> far   [+0xE68]
004F9A2D  C20800            ret  8
```
`0x004F9A30` = the reverse, `sBioCamera::PopOrg(Camera* dst, int index)` (52 instructions):
```
004F9A4E  8B0D68047D01      mov  ecx,ds:[0x17D0468]         ; a manager global
004F9A54  57                push edi                        ; index
004F9A55  E8A6740900        call 0x00590F00                 ; "is this entry valid?"
004F9A62  0F57C0            xorps xmm0,xmm0
004F9A6A  D9841830100000    fld  dword ptr [eax+ebx+0x1030] ; -> [esi+0x50] cameraPos
004F9A93  D90419            fld  dword ptr [ecx+ebx]        ; (index+0x41)*0x40 = 0x1040 -> [esi+0x70]
004F9AAC  D98050100000      fld  dword ptr [eax+0x1050]     ; -> [esi+0x60] cameraUp
004F9ACC  D98060100000      fld  dword ptr [eax+0x1060]     ; -> [esi+0x4C] fov
004F9AD5  D98064100000      fld  dword ptr [eax+0x1064]     ; -> [esi+0x44] near
004F9ADE  D98068100000      fld  dword ptr [eax+0x1068]     ; -> [esi+0x40] far
```
So the source object's pose block is **`+0x40` far, `+0x44` near, `+0x4C` fov, `+0x50` cameraPos,
`+0x60` cameraUp, `+0x70` targetPos** — identical to the entry layout of both arrays.

---

## 3. **MEASURED: `src` = a `uCameraCtrl` instance** (identified through the engine's own metadata)

`0x4F9950`'s four callers are `0x60C9F0`, `0x60CA60`, `0x60CAC7`, `0x60CB1E` — alias entries into
one function body in the `uCameraCtrl` code range:
```
0060C9F0  83EC20            sub  esp,0x20
0060C9F3  56                push esi
0060C9F4  8BF1              mov  esi,ecx                  ; this
0060C9F6  8B06              mov  eax,[esi]
0060C9F8  8B505C            mov  edx,[eax+0x5C]
0060C9FB  FFD2              call edx                      ; own vtable slot 23
...
0060CBE1  8B3D3CE28601      mov  edi,ds:[0x186E23C]       ; THE sBioCamera SINGLETON
0060CBE7  E874914300        call 0x00A45D60               ; ecx=[esi+0x80] -> slot index
0060CBEC  0FB6C8            movzx ecx,al
0060CBEF  51                push ecx                      ; arg2 = index
0060CBF0  56                push esi                      ; arg1 = src = THIS object
0060CBF1  8BCF              mov  ecx,edi                  ; this = sBioCamera
0060CBF3  E858CDEEFF        call 0x004F9950               ; PushOrg(src, index)
0060CBF8  8B969C4A0000      mov  edx,[esi+0x4A9C]
0060CBFE  8B0D3CE28601      mov  ecx,ds:[0x186E23C]
0060CC04  52                push edx
0060CC05  56                push esi
0060CC06  E825CEEEFF        call 0x004F9A30               ; PopOrg(src, [esi+0x4A9C])
0060CC0D  8BCE              mov  ecx,esi
0060CC13  E9E8F1FEFF        jmp  0x005FBE00               ; tail call, see below
```
`0x60C9F0` is slot 9 (vtable `+0x24`) of **vtable `0x152D620`**, and slot 4 of that table is an
MtDti getter (`mov eax, 0x017D26F0; ret`) for **`uCameraCtrl`** (registered size `0x4B70`):
```
=== vtable 0x0152D620
   slot  4 (+0x10)  0x0060AA60  2 instrs   <-- MtDti getter for uCameraCtrl (0x017D26F0)
   slot  9 (+0x24)  0x0060C9F0  158 instrs  <-- update
   slot 18 (+0x48)  0x005F80B0  13 instrs   <-- GetViewMatrix  (§4)
   slot 21 (+0x54)  0x0060D380  722 instrs  <-- writes [this+0x50] and [this+0x70]
```
**MEASURED:** `ds:[0x186E23C]` is the `sBioCamera` singleton — `PushOrg`/`PopOrg` are called with
it as `this` and write/read `+0xE30`/`+0x1030`, and `[0x186E23C+0xCE0]` (mScreenType) and
`[0x186E23C+0x15A0]` (mDispCtrlFlag) are read through it in other code (836 references, 387
functions). It should equal the `this` the parent's probe logged (`1F47A060` / `1F62A060`) —
worth one runtime read to confirm (§10).

Where the pointer comes from (both paths exist):
```
0054635B  8B4704 / 8B ...     mov edx,[ecx+0x640]        ; stage object +0x640 = uCameraCtrl
00546361  8B0D3CE28601       mov ecx,ds:[0x186E23C]     ; sBioCamera
00546367  57 / E8 A4 33 FB FF push edx ; call 0x004F9710 ; sBioCamera::AttachCameraCtrl(ctrl)
                              ;   -> validates the object, then 0x60CD50(ctrl, 0.0f)
004DB986  8BB024330000       mov esi,[eax+0x3324]       ; player object +0x3324 = its camera
004DB9A6  E8A5D50100         call 0x004F8F50            ; sBioCamera::SlotOfPlayer(playerId)
004DB9B7  E8B4EBFFFF         call 0x004DA570            ; sBioCamera::SetCamera(slot, cam)
004DA594  69C990010000       imul ecx,ecx,0x190          ; in SetCamera:
004DA59A  89543134           mov  dword ptr [ecx+esi+0x34],edx   ; slot[k].camera = cam
```
so each slot record is `sBioCamera + 0x30 + 0x190*k`: `+0x00` pointer (checked non-null), `+0x04`
= the camera object, `+0x10` = a flag byte (see `0x4F9200`, which walks the 8 slots with stride
`0x190` and returns the k-th active slot).

**Answer to question 2:** `src` is the `uCameraCtrl` (vtable 0x152D620, DTI 0x017D26F0, 0x4B70
bytes). Its pointer arrives from the stage (`[stage+0x640]`, via `0x4F9710`) or from the player
(`[player+0x3324]`, via `0x4DB970`). Per frame, `+0x50/+0x60/+0x70/+0x40/+0x44/+0x4C` are written
by (a) the `uCameraCtrl` update chain itself — `0x60D380` (slot 21, called from `0x60C9F0`) and
the base commit `0x5FBE00` — and (b) `0x4F9A30` (`PopOrg`), which overwrites them with the pose the
`sBioCamera` has resolved for that slot (this is where quake/blur/scripted-camera results land).

---

## 4. **MEASURED: the per-frame path that produces the view matrix**

```
uCameraCtrl::update                 0x0060C9F0   (vtbl 0x152D620 slot 9)
   computes its own pose  +0x50 cameraPos / +0x60 cameraUp / +0x70 targetPos  (+0x4C fov)
   -> 0x004F9950  PushOrg(this, slot)          : pose into sBioCamera->mCameraOrg[slot]
   -> 0x004F9A30  PopOrg (this, [this+0x4A9C]) : resolved pose back into this+0x40..+0x78
   -> jmp 0x005FBE00 (uCamera-family slot 9)
        005FBE11  fld [esi+0x50] / fstp [esi+0xD0]   ; publish pose as Vector4
        005FBE37  fld [esi+0x70] / fstp [esi+0xE0]
        005FBE5A  fld [esi+0x60] / fstp [esi+0xF0]
        005FBE89  fld [esi+0x4C] / fstp [esi+0x100]  ; fov
        (and 0x005F8180, which fills the 24 frustum floats at +0x150..+0x1B0)

sBioCamera::Update                  0x00503880   (vtbl 0x151A380 slot 11, 894 instructions)
   ... 0x004FCB70 : mCameraOrg[0..7] -> +0x1030[0..7]              (call at 0x504920)
   loop over the 8 slots (counter 8, stride 0x190 from this+0x34; exits on a null slot):
        00503B1C  mov ebx,[esp+0xB8]        ; = this+0x34 (set at 0x503993: lea eax,[ebx+0x34])
        00503B29  mov dword ptr [esp+0xB0],8
        00503B34  mov edi,[ebx]             ; slot camera object
        00503B3E  mov edx,[edi]             ; its vtable
        00503B40  mov edx,[edx+0x48]        ; *** slot 18 = GetViewMatrix ***
        00503B43  lea eax,[esp+0x20]        ; destination matrix (stack local)
        00503B47  push eax
        00503B48  mov ecx,edi
        00503B4A  call edx
        00503B18  mov esi,[esp+0x68]        ; this
        00503B23  add esi,0x12C8            ; walk mViewportCamera[] with stride 0x60
        00504773  (writes 16 floats at [esi-0x18] = this+0x12B0+0x60*k, + 3 floats, + a flag)
        00504906  add ebx,0x190 / add esi,0x60 / dec counter / jne 0x503B34
   then 0x00500E20, and per slot 0x004FE9F0 / 0x00502F60 / 0x005032D0, then 0x004F9DF0
```

`0x005F80B0` in full — **the pose → view matrix step** (13 instructions, `ret 4`):
```
005F80B0  56              push esi
005F80B1  8B742408        mov  esi,[esp+8]        ; out (Matrix*)
005F80B5  8D4160          lea  eax,[ecx+0x60]     ; up
005F80B8  8D5170          lea  edx,[ecx+0x70]     ; target
005F80BB  50              push eax                ; arg2
005F80BC  83C150          add  ecx,0x50           ; pos
005F80BF  52              push edx                ; arg1
005F80C0  51              push ecx                ; arg0
005F80C1  8BCE            mov  ecx,esi            ; this = the output matrix
005F80C3  E8587C8700      call 0x00E6FD20         ; MakeViewMatrix(pos, target, up, out)
005F80C8  8BC6            mov  eax,esi
005F80CA  5E              pop  esi
005F80CB  C20400          ret  4
```
`0x00E6FD20` is the engine's look-at builder (163 instructions): `[ebp+8]` = eye, `[ebp+0xC]` =
target, `[ebp+0x10]` = up:
```
00E6FD20  55 8BEC 83E4F0 83EC40     prologue
00E6FD29  8B4508                    mov  eax,[ebp+8]              ; eye
00E6FD2C  F30F1000                  movss xmm0,[eax]              ; eye.x
00E6FD4C  8B450C                    mov  eax,[ebp+0xC]            ; target
00E6FD59  F30F5CDA                  subss xmm3,xmm2               ; forward = target - eye
00E6FD97  F30F51C9                  sqrtss xmm1,xmm1              ; |forward|
00E6FD9B  0F2FD1                    comiss xmm2,xmm1              ; epsilon ds:[0x15110A0]
00E6FDAD  F30F5ED1                  divss xmm2,xmm1               ; normalize
00E6FDC9  8B4510                    mov  eax,[ebp+0x10]           ; up
00E6FDDC  F30F59F7                  mulss xmm6,xmm7               ; right = up x forward
00E6FDE4  F30F5CD6                  subss xmm2,xmm6
```
**MEASURED:** the object that `sBioCamera::Update` obtains the view matrix from is the *slot camera
object* (`[sBioCamera+0x34+0x190*k]`), and for 8 of the camera classes that object's slot 18 is
`0x5F80B0`:
```
class                vtable      slot 9      slot 11     slot 18 (+0x48)
uCameraBlur          0x0152CA98  0x005FBE00  0x00EE2A50  0x005F80B0
uCameraQuake         0x0152CBF8  0x005FBE00  0x00EE2A50  0x005F80B0
uCameraFovQuake      0x0152D4F0  0x005FBE00  0x00EE2A50  0x005F80B0
uCameraMotionSdl     0x0152D7C8  0x005FBE00  0x00EE2A50  0x005F80B0
uCameraQFPS          0x0152D9E8  0x005FBE00  0x00EE2A50  0x005F80B0
uCameraVeh           0x0152E178  0x005FBE00  0x00EE2A50  0x005F80B0
uCameraCtrl          0x0152D620  0x0060C9F0  0x00EE2A50  0x005F80B0
uCamera (0x80 bytes) 0x0152C3C0  0x01097D40  0x00EE2A50  0x013F5EA6   <- own getter
uFreeCamera          0x016EE018  0x01097D40  0x00EE2A50  0x0109ADB0   <- own getter
uOrthoCamera         0x016F3CE0  0x01097D40  0x00EE2A50  0x0118E6B0   <- own getter
```
(`0x0109ADB0` and `0x0118E6B0` are themselves callers of `0xE6FD20`, i.e. the same pattern with
their own pose reads.)

The result is stored per slot in `mViewportCamera[k]` = `sBioCamera + 0x12A0 + 0x60*k`
(`+0x10` = 16 floats = the matrix, `+0x50/+0x54/+0x58` = fov/…, `+0x04` = a valid byte set to 1):
```
00504773  F30F104F4C        movss xmm1,[edi+0x4C]     ; the source camera's fov
00504778  F30F1156E8        movss [esi-0x18],xmm2     ; entry+0x10  matrix row 0
...
0050480E  F30F114620        movss [esi+0x20],xmm0     ; entry+0x50
00504813  F30F117624        movss [esi+0x24],xmm6     ; entry+0x54
00504818  F30F114E28        movss [esi+0x28],xmm1     ; entry+0x58  (fov)
0050481D  C646DC01          mov  byte ptr [esi-0x24],1 ; entry+0x04 = valid
```
Per-entry span 0x60 × 8 = 0x300 = exactly the gap between `mViewportCamera (+0x12A0)` and
`mDispCtrlFlag (+0x15A0)` in the property table — an independent confirmation of the 8-entry
structure and of the entry size.

**Independent MEASURED proof that `+0x50` is the *live* camera position** — the engine reads it
itself, through the slot array, in `0x54D730` (a camera consumer called from 0x54DC00):
```
0054D9C2  8B153CE28601      mov  edx,ds:[0x186E23C]     ; sBioCamera
0054D9C8  69C990010000      imul ecx,ecx,0x190          ; slot index
0054D9CE  8B541134          mov  edx,[ecx+edx+0x34]     ; slot camera object
0054D9D2  F30F104250        movss xmm0,[edx+0x50]       ; cameraPos.x
0054D9D7  F30F104A54        movss xmm1,[edx+0x54]
0054D9DC  F30F5C8EF4000000  subss xmm1,[esi+0xF4]       ; minus the actor's position
0054D9E4  F30F5C86F0000000  subss xmm0,[esi+0xF0]
0054D9EC  F30F105258        movss xmm2,[edx+0x58]
0054D9F1  F30F5C96F8000000  subss xmm2,[esi+0xF8]       ; -> squared distance
```

**INFERRED (strong):** the matrix the renderer uses is produced by this chain. Two live
possibilities which the static data cannot separate:
(i) the renderer calls the same getter `[cam+0x48]` itself, or
(ii) the renderer reads `mViewportCamera[k]+0x10`.
Either way the *pose* it descends from is `[cam+0x50/+0x60/+0x70]`, and the single point through
which every variant passes is `0x5F80B0`. Note that NO 16-byte (`movaps`/`movups`) access to the
whole `0x12A0..0x1620` region exists anywhere in the image, so if (ii) is true, the renderer must
form a pointer (`lea`) first — displacement-only scans cannot settle this. §10 lists the one-run
experiment that does.

---

## 5. Answers to the four questions

**1. `0x4FF9B0` and its neighbourhood.** MEASURED: only reference = vtable slot
(`.rdata 0x151A3A8`); it is `sBioCamera`'s vtable slot 10. Body = reset/initialiser (§1). Callees:
`0x00EFB770`, `0x004F9240`, `0x004F93A0`, `0x004FC940` — the last of which is in the same
`0x4F9xxx–0x4FCxxx` block as the pose helpers. Callers: none statically ⇒ **INFERRED**: it is
called from the engine's virtual path at stage build. It is **not** a per-frame camera update and
has no per-frame caller; the per-frame updates are `0x60C9F0` (uCameraCtrl) and `0x503880`
(sBioCamera), §4. Where the per-frame pose comes from: the camera object's own `+0x50/+0x60/+0x70`
block (§2, §3, §4).

**2. `src` in `0x4F9950`.** MEASURED: a `uCameraCtrl` instance (§3) — `this` of `0x60C9F0`, whose
class is proved by its vtable's MtDti getter (0x152D620 slot 4 → `0x017D26F0` = `uCameraCtrl`,
0x4B70 bytes). Its pointer comes from `[stage+0x640]` (`0x54635B` → `0x4F9710`) or
`[player+0x3324]` (`0x4DB986` → `0x4DA570` slot registration). `[src+0x40]` (far), `+0x44` (near),
`+0x4C` (fov), `+0x50` (pos), `+0x60` (up), `+0x70` (target) are written each frame by the update
chain (`0x60D380` = uCameraCtrl slot 21, called from `0x60C9F0`; `0x5FBE00` = base commit; and
`0x4F9A30` = the resolved pose coming back from the sBioCamera). [MEASURED for the writes;
**INFERRED** for "each frame" — see §10 experiment 1.]

**3. The per-frame path.** `0x60C9F0` (uCameraCtrl::update, vtbl 0x152D620 slot 9) →
`0x4F9950`/`0x4F9A30` (pose exchange with the sBioCamera) → `0x5FBE00` (publish pose + frustum) …
and `0x503880` (sBioCamera::Update, vtbl 0x151A380 slot 11) → `0x4FCB70` (pose-array commit) →
per slot `[cam+0x48]` = `0x5F80B0` (GetViewMatrix) → `0xE6FD20` (look-at builder) →
`mViewportCamera[k]` (`+0x12A0+0x60*k`, matrix at `+0x10`). The `sCamera`/`sBioCamera::Viewport`
angles from the old notes are **not** on this path: `sCamera`'s registered field table was never
recovered, and no code reads a matrix out of the `sCamera` part (offsets < 0xCE0) of the object.
`uCameraCtrl` (0x4B70) itself has **no** property table, which is why its pose offsets had to be
recovered from the two copy helpers instead of from DTI metadata.

**4. Hook-shaped answer.** See §0 and §6.

---

## 6. Ranked hooks for the MinHook x86 probe

Sizes/bytes are MEASURED from the image (first 12–16 bytes quoted). MinHook needs ≥5 bytes it can
relocate; all of the following satisfy that.

| # | target (VA / RVA) | prologue (measured) | convention / `this` | what to touch |
|---|---|---|---|---|
| **1** | **0x005F80B0 / 0x1F80B0** `uCamera-family GetViewMatrix(out)` | `56 8B 74 24 08 8D 41 60 8D 51 70 50 83 C1 50 52 51 8B CE` | thiscall; `ecx` = camera object; `[esp+4]` = `Matrix*` | rotate **`[ecx+0x70]`** around `[ecx+0x50]` by head yaw/pitch; optionally `[ecx+0x60]` for roll. Runs **before** the look-at build, so it cannot be overwritten later in the frame. |
| 2 | 0x004F9950 / 0x0F9950 `sBioCamera::PushOrg(cam, index)` | `53 8B 5C 24 0C 57 8B F9 83 FB 08` | thiscall, `ret 8`; `ecx` = sBioCamera singleton (= `[0x186E23C]`); arg1 = camera object, arg2 = slot | **Observation first**: gives the live camera pointer + slot index once per camera per frame, for free. Steering here is *not* recommended (it lands in `mCameraOrg`, which nothing renders). |
| 3 | 0x004F9A30 / 0x0F9A30 `sBioCamera::PopOrg(cam, index)` | `53 57 8B 7C 24 10 8B D9 83 FF 08` | thiscall, `ret 8`; same as above | post-call rewrite of `[arg1+0x50/0x60/0x70]` = steer the *published* pose (used by everything downstream, including the frustum and the "distance to camera" logic). Combine with #1 only if you know the frame order. |
| 4 | 0x00503880 / 0x103880 `sBioCamera::Update` (slot 11) | `55 8B EC 83 E4 F0 81 EC 34 01 00 00` | thiscall; `ecx` = sBioCamera singleton | **Counter/labelling only** (answers "is this per-frame"). Do not write blind into an 894-instruction function with 5 analysis aliases. |
| 5 | 0x0060C9F0 / 0x20C9F0 `uCameraCtrl::update` (slot 9) | `83 EC 20 56 8B F1 8B 06 8B 50 5C FF D2` | thiscall; `ecx`/`esi` = uCameraCtrl | frame counter + "whose camera is this". For steering after the original body you must handle the tail `jmp 0x5FBE00` at 0x60CC13 (a plain epilogue detour misses that path); a mid-hook at 0x60CC06 is cleaner but needs a manual patch. |
| 6 | 0x0060D380 / 0x20D380 `uCameraCtrl` slot 21 | `55 8B EC 83 E4 F0 81 EC A4 00 00 00` | thiscall; `ecx` = uCameraCtrl; two float args on the stack | called from `0x60C9F0` (0x60CB33) with (fov-ish, fov-ish); writes `[this+0x50]` and `[this+0x70]` — the last *controller* stage before the commit. |
| 7 | 0x00E6FD20 / 0xA6FD20 `MakeViewMatrix(eye, target, up, out)` | `55 8B EC 83 E4 F0 83 EC 40 8B 45 08` | `__fastcall`-ish: `ecx` = out matrix, `[ebp+8]/[ebp+0xC]/[ebp+0x10]` = pos/target/up | fallback steering point; also the cheapest way to *log the exact view basis* the engine builds, for verification. Note it is shared by 71 callers (actors, effects, cameras) — filter by caller if you hook it. |

Suggested probe sequence (cheapest decisive observations first):

1. hook #2 `0x4F9950` and log `(ecx, arg1, arg2, [arg1+0x50], [arg1+0x70])` — this both counts the
   per-frame rate and hands over the camera object pointer the parent's earlier probe never had.
2. hook #1 `0x5F80B0` and log `(ecx, out-matrix row 3)`, then *steer* `[ecx+0x70]` by a fixed
   ±15° and watch the picture. This is the decisive A/B: if the picture swings, the read path is
   confirmed end-to-end and head tracking is just "replace the constant swing with yaw".
3. Only if #2 shows no response, hook #7 `0xE6FD20` with a caller filter to see which matrices are
   actually built for the frame being rendered.

---

## 7. Dead ends — do not retry (all with the evidence that killed them)

1. **Writing `mCameraOrg[i]` (`+0xE30…+0x1028`) to steer the view.** MEASURED: the only reader of
   that whole array in 1.87M instructions is `0x4FCB70`, which copies it to `+0x1030`. The observed
   "clean write audit, no visible change" is explained, not unexplained. Head tracking must not
   write there (the parent's sweep steps 1 and 2 — `+0xE40`, `+0x1050` — are dead).
2. **"The object at `[sBioCamera+0x1030]`"** — there is no pointer there. `+0x1030` is the second
   pose array (floats). Sweep steps 4 and 5 wrote into the middle of that array (`+0x1030+0x20` and
   `+0x1030+0x30`), i.e. into `entry0.fov/near` and `entry0.cameraUp`, which nothing renders from.
3. **`0x4FF9B0` as "the per-frame mCameraOrg writer / last word before the renderer".** It is the
   vtable-slot-10 reset (§1); it runs twice per session.
4. **`IDirect3DDevice9::SetTransform` / `MultiplyTransform`.** MEASURED: `call dword ptr [reg+0xB0]`
   and `[reg+0xB4]` occur **zero** times in the image (the device is a COM object, so a real call
   would look like that). The view does not reach D3D9 that way, so "hook SetTransform to find the
   camera" is not available. (Consistent with the project's earlier "the constant stream is not the
   camera" finding, which this report neither confirms nor refutes.)
5. **Searching for `call dword ptr [reg+0x48]` to find the getter's callers.** MEASURED: 0 hits.
   MSVC loads the slot into a register first — `mov edx,[edx+0x48]; call edx` (217 such sites
   exist, `_work\scan_vcall.py`). Any xref/displacement search that assumes the memory-operand form
   will silently report "no caller" for every virtual function in this binary. (Same trap the
   README already recorded for the DTI registration page.)
6. **`mViewportCamera` read by displacement.** MEASURED: zero `movaps/movups/movdqa` accesses to
   `0x12A0..0x1620` in the whole image, and the `lea` hits in that range (`0x1300`, `0x1310`,
   `0x1320`, `0x1360`, `0x1370`, …) mostly belong to *other* classes whose own fields sit at the
   same displacements (e.g. `uCameraCtrl` is 0x4B70 bytes and uses 0x1300+ itself, see
   `0x60D380`/`0x5F5A40`). Displacement-only matching cannot decide this; use object identity.
7. **One-operand memory operands are reads, not stores.** The first version of the array scan missed
   `0x4F9A30`'s nine `fld [eax+0x1030…]` reads entirely because it only inspected `operands[1:]`.
   If a future scan of this kind reports "nobody reads array X", check that rule first.
8. **`sBioCamera::Update` has no static caller.** Neither a direct call xref nor any
   `mov reg, ds:[0x186E23C] … call [reg+0x2C]` site exists (both scans return 0). It is reached
   through some task/scheduler indirection the recursive-descent analysis cannot resolve. Do not
   spend another session on "who calls the camera update" — measure the call rate instead.

---

## 8. Offset/layout cheat sheet (all MEASURED unless marked)

```
sBioCamera singleton pointer        ds:[0x186E23C]           (836 refs, 387 fns)
  vtable 0x151A380  slot 8 = 0x4FA570 ctor | slot 9 = 0x4FA560 DTI getter
                    slot 10 = 0x4FF9B0 reset (§1) | slot 11 = 0x503880 Update (§4)
  +0x0CE0 mScreenType             +0x0D24/28/2C/30 viewport x,y,w,h
  +0x0D80 mCameraParam            +0x0E30 mCameraOrg[8] (stride 0x40)
                                    entry: +0x00 pos, +0x10 target, +0x20 up,
                                           +0x30 fov, +0x34 near, +0x38 far
  +0x1030 second pose array[8]    (same entry layout; written by 0x4FCB70 from mCameraOrg)
  +0x12A0 mViewportCamera[8] (stride 0x60)
                                    entry: +0x04 valid byte, +0x10 4x4 matrix (16 floats,
                                           ends at +0x4F), +0x50 = the source camera's fov
                                           (loaded from [cam+0x4C] at 0x504773). Whether
                                           +0x54/+0x58 are used is NOT observed.
  +0x15A0 mDispCtrlFlag  +0x15B0 mWipe  +0x1610/14/18 CameraPatch_*
  +0x0030 slot record base, stride 0x190:
                                    +0x00 pointer, +0x04 = camera object, +0x10 flag byte
uCameraCtrl  vtable 0x152D620, DTI 0x017D26F0, size 0x4B70
  +0x0040 far   +0x0044 near   +0x004C fov
  +0x0050 cameraPos   +0x0060 cameraUp   +0x0070 targetPos      <- THE LIVE POSE
  +0x00D0/+0x00E0/+0x00F0 published pos/target/up (0x5FBE00), +0x0100 fov, +0x0104 ?
  +0x0150..+0x01B0 frustum planes (0x5F8180)
  +0x04A68/0x04A9C/0x04AA0/0x04AB0/0x04AC0/0x04AD0 controller state (+0x4AB0/0x4AC0 =
      copies of its own pos/target, made in 0x60C9F0)
  slot 9 = 0x60C9F0 update | slot 18 = 0x5F80B0 GetViewMatrix | slot 21 = 0x60D380
uCamera family slot 18 shared by 8 classes (§4); uCamera/uFreeCamera/uOrthoCamera override it.
MakeViewMatrix(eye, target, up, out) = 0x00E6FD20, 71 callers, 163 instructions.
```

---

## 9. What is still unknown (and cannot be settled offline)

1. Whether the renderer calls `[cam+0x48]` itself or reads `mViewportCamera[k]+0x10`. Both descend
   from the same pose, so hook #1 works either way; experiment 2 in §10 distinguishes them.
2. Whether `0x60C9F0` / `0x503880` really run once per frame. The analysis cannot prove call
   frequency (**INFERRED** from the design and from the measured "the engine rewrites the pose
   between frames" behaviour). Two counter hooks answer it.
3. Which slot index belongs to the local player's camera. `0x4F9200` (k-th active slot) and
   `0x4F8F50` (`SlotOfPlayer`, iterating `[this+0xCF0+4*i]`, i<2) give the mapping; the probe can
   simply log `arg2` of `0x4F9950` together with the camera pointer and compare positions with the
   player's world position.

## 10. Two cheap runtime experiments that close the loop

1. **Confirm the singleton and the slot array.** Read `[0x186E23C]` and compare with the `this`
   the existing `camhook` logs for `0x4FF9B0` (both should be e.g. `1F47A060`). Then dump
   `[[0x186E23C]+0x34+0x190*k]` for k=0..7 and, for each non-null pointer, read `+0x50/+0x60/+0x70`
   and `[[p]]` (its vtable) — a vtable of `0x152D620` identifies the gameplay `uCameraCtrl`
   immediately. Also dump `[[0x186E23C]+0x12A0+0x60*k+0x10 … +0x4C]` while walking: if those 16
   floats track the player's view, `mViewportCamera` is the renderer's source (variant (ii)).
2. **Swing `[cam+0x70]` inside hook #1** (rotate around `[cam+0x50]` by ±15° about world Y, same
   code as the existing `sweep_tick`, but applied to `ecx+0x70` of the hooked camera object). One
   run decides everything: the pose is the render source, and the same hook becomes the head
   tracking implementation (replace sin(t) with the headset yaw).

---

### Scratch tooling produced by this investigation (in `_work\`, all UTF-8)

| file | purpose |
|---|---|
| `dv.py` | function/range/caller dump helper over the analysis cache |
| `scan_readwrite.py` | corrected read/write scan of the two 8×0x40 pose arrays |
| `list_touchers.py` | every instruction touching `+0xE30` or `+0x1030`, grouped by function |
| `scan_entry_array.py` | accesses to a strided entry array (`+0x12A0`, stride 0x60) |
| `scan_viewportcam.py` | float/vector readers of `mViewportCamera` |
| `scan_pose_readers.py`, `scan_pose_writers.py` | readers/writers of a `+0x50/+0x60/+0x70` pose block |
| `scan_pose_block.py` | readers/writers of the published `+0xD0/+0xE0/+0xF0` block |
| `scan_lea_ptr.py`, `scan_imm.py` | pointer-taking (`lea`) and stride (0x190 / 0x60) searches |
| `scan_vcall.py` | `mov reg,[obj+SLOT]; call reg` virtual-call sites (the form MSVC actually emits) |
| `which_slot.py`, `dump_vtable.py`, `slot18_map.py` | vtable slot identification and camera-class slot 18 map |
| `scan_global_user.py` | uses of the sBioCamera global `[0x186E23C]` as a base |
| `find_matrix_writers.py`, `find_srcclass.py`, `show_fields.py`, `find_viewupload.py` | matrix-run writers, class-layout matching, field tables, d3d9 import checks |
