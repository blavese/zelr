/* User programs live in their own half of the address space, in a top level
   page table entry the kernel does not share. See USER_SPACE_BASE. */
ENTRY(_start)
SECTIONS {
    . = 0x8040000000;
    .text : ALIGN(4K) { *(.text._start) *(.text*) }
    .rodata : ALIGN(4K) { *(.rodata*) }
    .data : ALIGN(4K) { *(.data*) }
    .bss : ALIGN(4K) { *(COMMON) *(.bss*) }
}
