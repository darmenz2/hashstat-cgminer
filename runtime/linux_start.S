/* SPDX-License-Identifier: GPL-3.0-only */


.syntax unified
.arm
.section .text
.global _start
.type _start,%function
_start:
    bic sp, sp, #7
    bl hs_linux_selftest_main
    mov r7, #1
    svc #0
1:  b 1b
.size _start, .-_start

.global hs_linux_write
.type hs_linux_write,%function
hs_linux_write:
    push {r7, lr}
    mov r7, #4
    svc #0
    pop {r7, pc}
.size hs_linux_write, .-hs_linux_write
.section .note.GNU-stack,"",%progbits
