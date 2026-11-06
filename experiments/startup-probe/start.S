/* RAM-only probe entry. No flash, PLL, GPIO, RF, or PA writes. */
    .section .text.entry,"ax",@progbits
    .global _start
    .type _start,@function
_start:
    psrclr ie, fe
    lrw r7, 0x00050000
    mov r14, r7
    bsr probe_main
1:  br 1b
    .size _start, .-_start
