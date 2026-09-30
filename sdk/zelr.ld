/* User programs live in their own half of the address space, in a top level
   page table entry the kernel does not share. See USER_SPACE_BASE.

   The writable data is marked, start and end, for a collector that looks
   through it (zelr.h). Programs are built for the large code model, whose
   data is .ldata and .lbss: named here, or it lands after the end mark. */
ENTRY(_start)
SECTIONS {
    . = 0x8040000000;
    .text : ALIGN(4K) { *(.text._start) *(.text*) }
    .rodata : ALIGN(4K) { *(.rodata*) }
    .data : ALIGN(4K) { __zelr_data_start = .; *(.data*) *(.ldata*) }
    .bss : ALIGN(4K) { *(COMMON) *(.bss*) *(LARGE_COMMON) *(.lbss*) __zelr_data_end = .; }
}
