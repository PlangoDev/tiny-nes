/* TINY-32: the program at 0, then its constants, data and zeroed data; the heap from _end up to the stack. */
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : ALIGN(4) { *(.rodata .rodata.* .srodata .srodata.*) }
    .data : ALIGN(4) { *(.data .data.* .sdata .sdata.*) }
    .bss : ALIGN(4) { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) . = ALIGN(4); __bss_end = .; }
    _end = .;
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame*) }
}
