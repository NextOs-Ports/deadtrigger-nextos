/* setjmp/longjmp/sigsetjmp/siglongjmp bridge for a Bionic ARM32 guest.
 *
 * The guest reserves a Bionic jmp_buf of 64 words (256 bytes) and a sigjmp_buf
 * of 65 words (260 bytes). The device glibc always writes its flag at env+256
 * and, with savemask=1, another 128 bytes from env+260 on. So even _setjmp is
 * unsafe for a plain guest jmp_buf.
 *
 * We store exactly the AAPCS-preserved context (r4-r12, sp, lr, d8-d15 and
 * FPSCR) in the first 120 bytes, plus the Bionic flag/mask. Nothing written
 * ever exceeds the smaller buffer. The non-"sig" entry points use Bionic
 * semantics: setjmp saves the mask, _setjmp does not. Everything is assembly
 * because a C wrapper would build a frame already returned from, which longjmp
 * would try to return into.
 *
 * Mask helpers ff5_sigjmp_save_mask / ff5_sigjmp_restore_mask are provided by
 * bionic_shim.c (no malloc/locks/logging: siglongjmp may run inside a handler).
 */

    .syntax unified
    .arm
    .text
    .align  2

    .fpu    vfpv3

    .equ FF5_JMP_FLAG,   0
    .equ FF5_JMP_MASK,   4
    .equ FF5_JMP_VFP,    8
    .equ FF5_JMP_FPSCR, 72
    .equ FF5_JMP_CORE,  76
    .equ FF5_JMP_SP,   112
    .equ FF5_JMP_LR,   116

    .global ff5_setjmp
    .type   ff5_setjmp, %function
ff5_setjmp:
    mov     r1, #1
    b       ff5_sigsetjmp
    .size   ff5_setjmp, .-ff5_setjmp

    .global ff5__setjmp
    .type   ff5__setjmp, %function
ff5__setjmp:
    mov     r1, #0
    b       ff5_sigsetjmp
    .size   ff5__setjmp, .-ff5__setjmp

    .global ff5_sigsetjmp
    .type   ff5_sigsetjmp, %function
ff5_sigsetjmp:
    /* Save the caller context BEFORE calling any helper. */
    add     r2, r0, #FF5_JMP_VFP
    vstmia  r2, {d8-d15}
    vmrs    r3, fpscr
    str     r3, [r0, #FF5_JMP_FPSCR]
    add     r2, r0, #FF5_JMP_CORE
    stmia   r2, {r4-r12}
    str     sp, [r0, #FF5_JMP_SP]
    str     lr, [r0, #FF5_JMP_LR]

    cmp     r1, #0
    movne   r2, #1
    moveq   r2, #0
    str     r2, [r0, #FF5_JMP_FLAG]
    beq     1f

    push    {r0, r1, r2, lr}
    add     r0, r0, #FF5_JMP_MASK
    bl      ff5_sigjmp_save_mask
    mov     r3, r0
    pop     {r0, r1, r2, lr}
    cmp     r3, #0
    moveq   r3, #1
    movne   r3, #0
    str     r3, [r0, #FF5_JMP_FLAG]
1:
    mov     r0, #0
    bx      lr
    .size   ff5_sigsetjmp, .-ff5_sigsetjmp

    /* Bionic __sigsetjmp(env, savesigs): alias to sigsetjmp */
    .global ff5___sigsetjmp
    .type   ff5___sigsetjmp, %function
ff5___sigsetjmp:
    b       ff5_sigsetjmp
    .size   ff5___sigsetjmp, .-ff5___sigsetjmp

    .global ff5_longjmp
    .type   ff5_longjmp, %function
ff5_longjmp:
    b       ff5_siglongjmp
    .size   ff5_longjmp, .-ff5_longjmp

    .global ff5__longjmp
    .type   ff5__longjmp, %function
ff5__longjmp:
    b       ff5_siglongjmp
    .size   ff5__longjmp, .-ff5__longjmp

    .global ff5_siglongjmp
    .type   ff5_siglongjmp, %function
ff5_siglongjmp:
    ldr     r2, [r0, #FF5_JMP_FLAG]
    cmp     r2, #0
    beq     2f
    push    {r0, r1, r2, lr}
    add     r0, r0, #FF5_JMP_MASK
    bl      ff5_sigjmp_restore_mask
    pop     {r0, r1, r2, lr}
2:
    /* Load SP/LR before restoring the registers. r2/r3 are caller-saved and are
       not part of the AAPCS-preserved context. */
    ldr     r2, [r0, #FF5_JMP_SP]
    ldr     r3, [r0, #FF5_JMP_LR]
    add     r12, r0, #FF5_JMP_VFP
    vldmia  r12, {d8-d15}
    ldr     r12, [r0, #FF5_JMP_FPSCR]
    vmsr    fpscr, r12
    cmp     r1, #0
    moveq   r1, #1
    add     r12, r0, #FF5_JMP_CORE
    ldmia   r12, {r4-r12}
    mov     r0, r1
    mov     sp, r2
    bx      r3
    .size   ff5_siglongjmp, .-ff5_siglongjmp

    .section .note.GNU-stack,"",%progbits
