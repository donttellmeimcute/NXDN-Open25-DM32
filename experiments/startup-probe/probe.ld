/* Link only a standalone RAM test, never a vendor update or flash image. */
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x00010000, LENGTH = 0x1000 }
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) } > RAM
    .data : { *(.data*) } > RAM
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(SIZEOF(.bss) == 0, "Probe must not depend on uninitialized BSS")
