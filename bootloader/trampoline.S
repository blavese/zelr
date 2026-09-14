/* Where a second processor starts.
 *
 * An application processor comes out of reset in 16-bit real mode at a
 * physical address the boot processor chose, which must be page aligned and
 * below a megabyte. It has no stack, no descriptor tables and no paging: it
 * is a fresh machine that happens to share memory with a running one.
 *
 * This is what it lands on. It is a flat binary rather than part of the
 * kernel because it has to be copied to a fixed low address and every
 * address inside it has to be absolute, which is exactly the situation the
 * bootloader is in, so it is built the same way.
 *
 * Getting to long mode from here is the same sequence boot.S uses, and for
 * the same reason: the processor will not enter it without paging already
 * on. The difference is that the tables already exist. The boot processor
 * hands over its own, so this one shares the kernel's address space from its
 * first instruction rather than building a second copy of it.
 *
 * The boot processor fills in the parameter block below before sending the
 * startup signal: which page tables to use, which stack this processor gets,
 * and where in the kernel to go. Startup is serialised, so one slot is
 * enough.
 */
.code16
.section .text
.globl _start

.set BASE, 0x8000

_start:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /* PAE first: long mode will not engage without it. */
    movl    %cr4, %eax
    orl     $(1 << 5), %eax
    movl    %eax, %cr4

    /* The boot processor's page tables, so this processor sees the kernel
       exactly as the one that started it does. */
    movl    BASE + (param_cr3 - _start), %eax
    movl    %eax, %cr3

    /* Ask for long mode. */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $(1 << 8), %eax
    wrmsr

    /* Protected mode and paging go on in the same write. Paging cannot be
       enabled while the processor is still in real mode, and this one starts
       there, so setting the two bits separately would leave the first write
       either ignored or faulting. boot.S does not have this problem because
       a multiboot loader has already entered protected mode for it. */
    movl    %cr0, %eax
    orl     $((1 << 31) | 1), %eax
    movl    %eax, %cr0

    lgdt    BASE + (gdt_desc - _start)
    ljmpl   $0x08, $(BASE + (long_mode - _start))

.code64
long_mode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    /* Its own stack, handed over by whoever started it. */
    movq    BASE + (param_stack - _start), %rsp
    xorq    %rbp, %rbp

    /* Say which processor this is, then leave the trampoline for good. The
       argument goes in rdi, which is where a System V function looks. */
    movq    BASE + (param_arg - _start), %rdi
    movq    BASE + (param_entry - _start), %rax
    call    *%rax

    /* The entry point is not expected to return. */
1:  hlt
    jmp     1b

.align 8
gdt:
    .quad   0
    /* code: present, ring 0, long mode. No base, no limit: in 64-bit the
       processor ignores both. */
    .quad   0x00209A0000000000
    .quad   0x0000920000000000
gdt_end:

gdt_desc:
    .word   gdt_end - gdt - 1
    .quad   BASE + (gdt - _start)

/* Filled in by smp.c before each processor is started. The signature lets it
   find them without knowing how the code above is laid out. Each is a full
   machine word now, because a page table address and a kernel entry point
   are both 64-bit. */
.align 8
    .ascii  "ZELRSMP1"
param_cr3:      .quad 0
param_stack:    .quad 0
param_entry:    .quad 0
param_arg:      .quad 0
