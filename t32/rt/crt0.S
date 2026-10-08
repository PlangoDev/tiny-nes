# TINY-32's start: a stack at the top of RAM, .bss cleared, then main(); its return value stops the machine.
    .section .text.start
    .globl _start
_start:
    li sp, 0x02000000 - 16
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call main
    li t0, 0xF0000010
    sw a0, 0(t0)
3:  j 3b

