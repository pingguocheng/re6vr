; Ground-truth corpus for the x86 decoder.
;
; Built by scripts/tests/groundtruth.py via ml.exe, then disassembled by dumpbin /disasm.
; The pair (dumpbin's instruction bytes, dumpbin's instruction text) is the oracle: the
; decoder under test must consume exactly the same number of bytes and render the same
; instruction for every line. Nothing here is hand-transcribed, so a table typo cannot hide
; behind a matching typo in the expectation.
;
; Deliberately covered: every addressing form (base, index*scale, disp8, disp32, absolute),
; the segment overrides, the immediate widths, all conditional branches, the group opcodes,
; and the SSE forms a 2005-era DirectX 9 game actually uses.

.686
.XMM
.model flat, C
option casemap:none
; The fixture deliberately uses fs:/gs: overrides (SEH frames, TLS). MASM otherwise refuses
; to touch a segment register it has no assumption for.
assume fs:nothing, gs:nothing

_DATA SEGMENT
jump_table dd test_alu, test_mem, test_flow, test_sse, test_misc, test_x87
some_string db "hello from the fixture", 0
align 4
some_floats dd 03F800000h, 040000000h, 040400000h, 040800000h
_DATA ENDS

_TEXT SEGMENT

test_alu proc
    add     eax, ebx
    add     al, bl
    add     ecx, 1234h
    add     edx, 7
    adc     eax, ebx
    sub     esp, 4
    sbb     eax, eax
    and     ecx, 0FFFFh
    or      edx, 10h
    xor     eax, eax
    cmp     dword ptr [eax], 0
    cmp     byte ptr [esi], 5
    test    eax, eax
    test    byte ptr [edi+8], 1
    not     eax
    neg     ecx
    inc     dword ptr [ebx]
    dec     esi
    mul     ecx
    imul    eax, ecx
    imul    eax, ecx, 100h
    imul    edx, dword ptr [ebp-4], 3
    div     ebx
    idiv    dword ptr [esp+16]
    shl     eax, 1
    shr     ecx, cl
    sar     edx, 3
    rol     eax, 7
    ror     ebx, cl
    shld    eax, ebx, 5
    shrd    eax, ebx, cl
    xchg    eax, ecx
    xchg    edx, dword ptr [esi]
    xadd    dword ptr [edi], eax
    cmpxchg dword ptr [ebx], ecx
    bt      eax, 3
    bts     dword ptr [esi], edx
    bsf     eax, ecx
    bsr     edx, dword ptr [edi+4]
    bswap   eax
    movzx   eax, byte ptr [esi]
    movzx   ecx, word ptr [edx+2]
    movsx   edx, byte ptr [edi]
    movsx   eax, word ptr [ebx]
    setne   al
    sete    byte ptr [esp+4]
    cmove   eax, ebx
    cmovne  ecx, dword ptr [esi]
    ret
test_alu endp

test_mem proc
    mov     eax, dword ptr [eax]
    mov     ecx, dword ptr [esp+16]
    mov     edx, dword ptr [ebp-24]
    mov     ebx, dword ptr [eax+ecx*4]
    mov     esi, dword ptr [eax+ecx*4+32]
    mov     edi, dword ptr [eax*2+1000h]
    mov     eax, dword ptr [1000h]
    mov     ecx, dword ptr ds:[1000h]
    mov     eax, dword ptr fs:[0]
    mov     edx, dword ptr gs:[8]
    lea     eax, [ebx+ecx*8+16]
    lea     ecx, [ebp-64]
    movsx   eax, byte ptr [esi+ecx*2-1]
    mov     word ptr [edi], 1234h
    mov     byte ptr [ebx+2], 7Fh
    pop     dword ptr [eax]
    push    dword ptr [ecx]
    lea     edx, [eax+eax*4]
    lea     esi, [ecx*4+8]
    mov     dword ptr [esp+ecx*4], eax
    mov     dword ptr [ebx+ecx*2-16], edx
    mov     eax, dword ptr [12345678h]
    mov     dword ptr [12345678h], ecx
    ret
test_mem endp

test_flow proc
    push    ebp
    mov     ebp, esp
    sub     esp, 64
loc_start:
    cmp     eax, 10
    je      loc_je
    jne     loc_jne
    jl      loc_jl
    jle     loc_jle
    jg      loc_jg
    jge     loc_jge
    jb      loc_jb
    jbe     loc_jbe
    ja      loc_ja
    jae     loc_jae
    js      loc_js
    jns     loc_jns
    jo      loc_jo
    jno     loc_jno
    jp      loc_jp
    jnp     loc_jnp
    jmp     loc_jmp
loc_je:
    mov     eax, 1
loc_jne:
    mov     eax, 2
loc_jl:
    mov     eax, 3
loc_jle:
    mov     eax, 4
loc_jg:
    mov     eax, 5
loc_jge:
    mov     eax, 6
loc_jb:
    mov     eax, 7
loc_jbe:
    mov     eax, 8
loc_ja:
    mov     eax, 9
loc_jae:
    mov     eax, 10
loc_js:
    mov     eax, 11
loc_jns:
    mov     eax, 12
loc_jo:
    mov     eax, 13
loc_jno:
    mov     eax, 14
loc_jp:
    mov     eax, 15
loc_jnp:
    mov     eax, 16
loc_jmp:
    nop
    call    test_alu
    call    dword ptr [eax+8]
    call    ecx
    jmp     dword ptr [edx+12]
    jmp     esi
    push    offset loc_start
    push    0
    push    12345678h
    push    eax
    pushad
    popad
    leave
    ret     8
test_flow endp

test_sse proc
    movups  xmm0, xmmword ptr [eax]
    movups  xmmword ptr [ecx+16], xmm1
    movaps  xmm2, xmm3
    movaps  xmmword ptr [edx], xmm4
    movss   xmm0, dword ptr [esi]
    movss   dword ptr [edi+4], xmm1
    movsd   xmm2, qword ptr [eax+8]
    addps   xmm0, xmm1
    subps   xmm2, xmm3
    mulps   xmm4, xmm5
    divps   xmm6, xmm7
    addss   xmm0, xmm1
    subss   xmm2, dword ptr [eax]
    mulss   xmm3, dword ptr [ecx+4]
    divss   xmm4, xmm5
    addsd   xmm0, xmm1
    subsd   xmm2, qword ptr [edx]
    mulsd   xmm3, xmm4
    divsd   xmm5, xmm6
    sqrtps  xmm0, xmm1
    sqrtss  xmm2, xmm3
    sqrtsd  xmm4, xmm5
    maxps   xmm0, xmm1
    minps   xmm2, xmm3
    maxss   xmm4, xmm5
    minsd   xmm6, xmm7
    andps   xmm0, xmm1
    andnps  xmm2, xmm3
    orps    xmm4, xmm5
    xorps   xmm6, xmm7
    andpd   xmm0, xmm1
    xorpd   xmm2, xmm3
    unpcklps xmm0, xmm1
    unpckhps xmm2, xmm3
    shufps  xmm0, xmm1, 1Bh
    cmpps   xmm2, xmm3, 0
    cmpsd   xmm4, xmm5, 1
    cmpss   xmm6, xmm7, 2
    comiss  xmm0, xmm1
    ucomiss xmm2, dword ptr [eax]
    comisd  xmm3, qword ptr [ecx]
    cvtsi2ss xmm0, eax
    cvtsi2ss xmm1, dword ptr [ebx]
    cvttss2si eax, xmm0
    cvtss2si ecx, dword ptr [edx]
    cvtsi2sd xmm2, ecx
    cvttsd2si edx, xmm3
    cvtdq2ps xmm0, xmm1
    cvtps2dq xmm2, xmm3
    cvttps2dq xmm4, xmm5
    cvtdq2pd xmm6, xmm7
    movd    xmm0, eax
    movd    eax, xmm1
    movd    xmm2, dword ptr [esi]
    movd    dword ptr [edi], xmm3
    movdqa  xmm0, xmm1
    movdqu  xmm2, xmm3
    movdqu  xmmword ptr [eax], xmm4
    movmskps eax, xmm1
    movmskpd ecx, xmm2
    pxor    xmm0, xmm1
    por     xmm2, xmm3
    pand    xmm4, xmm5
    pandn   xmm6, xmm7
    paddb   xmm0, xmm1
    paddw   xmm2, xmm3
    paddd   xmm4, xmm5
    paddq   xmm6, xmm7
    psubb   xmm0, xmm1
    psubd   xmm2, xmm3
    pmullw  xmm4, xmm5
    pmuludq xmm6, xmm7
    pcmpeqb xmm0, xmm1
    pcmpeqd xmm2, xmm3
    pcmpgtw xmm4, xmm5
    punpcklbw xmm6, xmm7
    packssdw xmm0, xmm1
    packuswb xmm2, xmm3
    psllw   xmm0, 4
    psrld   xmm1, 8
    psllq   xmm2, xmm3
    pshufd  xmm0, xmm1, 1Bh
    pshufhw xmm2, xmm3, 0
    pshuflw xmm4, xmm5, 0FFh
    pinsrw  xmm0, eax, 2
    pextrw  eax, xmm1, 3
    pmovmskb ecx, xmm2
    movq    xmm0, qword ptr [eax]
    movq    qword ptr [ebx+8], xmm1
    movq    mm0, mm1
    emms
    ret
test_sse endp

test_misc proc
    nop
    cpuid
    rdtsc
    pushfd
    popfd
    cld
    std
    clc
    stc
    cmc
    sahf
    lahf
    cwde
    cdq
    xlatb
    int     3
    mov     eax, 12345678h
    mov     ecx, 0
    mov     edx, 0FFFFFFFFh
    mov     ebx, 80000000h
    mov     esi, 7Fh
    mov     edi, 80h
    enter   16, 0
    leave
    ret
test_misc endp

test_x87 proc
    fld     dword ptr [eax]
    fst     dword ptr [ecx]
    fstp    dword ptr [edx]
    fld     qword ptr [ebx]
    fstp    qword ptr [esi]
    fadd    dword ptr [edi]
    fsub    dword ptr [eax+4]
    fmul    dword ptr [ecx+8]
    fdiv    dword ptr [edx+12]
    faddp   st(1), st(0)
    fmulp   st(1), st(0)
    fsubp   st(1), st(0)
    fdivp   st(1), st(0)
    fld     st(0)
    fxch    st(1)
    fucom   st(1)
    fucomp  st(2)
    fucompp
    fcomi   st(0), st(1)
    fcomip  st(0), st(2)
    fldcw   word ptr [eax]
    fnstcw  word ptr [ecx]
    fnstsw  ax
    fwait
    fld1
    fldz
    fabs
    fchs
    fsqrt
    fsin
    fcos
    fptan
    fpatan
    frndint
    fscale
    f2xm1
    fyl2x
    fldpi
    finit
    fincstp
    fdecstp
    ret
test_x87 endp

test_prefixed proc
    lock add dword ptr [eax], 1
    lock inc dword ptr [ecx]
    lock xchg dword ptr [edx], eax
    rep movsd
    rep stosd
    repne scasb
    repe cmpsb
    movsb
    stosb
    lodsb
    mov     eax, dword ptr fs:[0]
    mov     dword ptr fs:[0], eax
    ret
test_prefixed endp

_TEXT ENDS

END
