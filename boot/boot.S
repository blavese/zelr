/* Getting into 64-bit long mode.
 *
 * There are two ways into this kernel and this file is one of them. A
 * multiboot loader, which is what QEMU's -kernel is, hands control over in
 * 32-bit protected mode with paging off. Long mode cannot be entered from
 * there directly: the processor requires paging to be on before the switch,
 * and paging in long mode means four levels of page tables that have to
 * exist before they can be turned on. So the order is fixed and awkward:
 * build tables, turn on PAE, ask for long mode, turn on paging, and only
 * then jump through a 64-bit descriptor.
 *
 * The other way in is uefi/loader.c, which arrives already in long mode
 * because the firmware got there first, and lands at _start64 instead.
 *
 * The tables built here identity map the first 4 GiB with 2 MiB pages. That
 * is enough to reach a framebuffer the firmware put at 2 GiB and the local
 * APIC just under 4, which are the two things above the kernel that get
 * touched before paging.c takes over.
 */

/* The very front of the flat image. A loader that is not a multiboot loader
   has no way to know where to jump, and the first bytes of the image are the
   multiboot header rather than code, so the entry point is stated here. */
.section .kheader
.align 16
.ascii "ZELRKRN"
.byte 0
.quad _start64                   /* for a loader already in long mode */
.quad _start32                   /* for one still in 32-bit protected mode */

.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set KLUDGE,   1<<16              /* the addresses below are authoritative */
.set FLAGS,    ALIGN | MEMINFO | KLUDGE
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

/* The a.out kludge, which is what makes a 64-bit kernel loadable by a
   multiboot loader at all.
 *
 * A multiboot loader normally parses the ELF headers, and every one of them
 * refuses a 64-bit image because multiboot predates long mode. With these
 * fields set the loader stops looking at the ELF entirely and does what it is
 * told: copy the flat file to load_addr, zero up to bss_end_addr, jump to
 * entry_addr. The file it is given is the flattened image rather than the
 * ELF, and the entry is the 32-bit trampoline below.
 *
 * That is why build.sh produces both. */
.section .multiboot
.align 4
multiboot_header:
.long MAGIC
.long FLAGS
.long CHECKSUM
.long multiboot_header           /* where this header is */
.long __kernel_start             /* where the image begins */
.long 0                          /* load to the end of the file */
.long __bss_end                  /* zero the rest */
.long _start

.section .bss
.align 4096
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4096 * 4          /* four of them: 4 GiB in 2 MiB pages */
.align 16
stack_bottom:
.skip 65536
stack_top:

.section .rodata
.align 16
/* A descriptor table with one 64-bit code segment, which is all that is
   needed to make the jump. gdt.c installs the real one later. */
gdt64:
    .quad 0
gdt64_code:
    /* present, ring 0, code, long mode */
    .quad 0x00209A0000000000
gdt64_data:
    .quad 0x0000920000000000
gdt64_end:
gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    mov $stack_top, %esp
    xor %ebp, %ebp

    /* The multiboot loader leaves its magic in eax and its information
       structure in ebx. Both are needed after the switch, and long mode
       clears the top half of every register on the way in, so park them
       somewhere memory can hold. */
    mov %eax, mb_magic
    mov %ebx, mb_info
    movl $0, from_handoff
    jmp go_long

/* Where a 32-bit bootloader of ours arrives, having built a handoff
   structure itself and left a pointer to it in ebx. */
.global _start32
.type _start32, @function
_start32:
    cli
    mov $stack_top, %esp
    xor %ebp, %ebp

    mov %ebx, mb_info
    movl $1, from_handoff

go_long:
    call check_long_mode
    call build_tables

    /* PAE has to be on before long mode will engage. */
    mov %cr4, %eax
    or $(1 << 5), %eax
    mov %eax, %cr4

    mov $pml4, %eax
    mov %eax, %cr3

    /* Ask for long mode in the extended feature register. */
    mov $0xC0000080, %ecx
    rdmsr
    or $(1 << 8), %eax
    wrmsr

    /* Turning paging on is what actually enters long mode. */
    mov %cr0, %eax
    or $(1 << 31), %eax
    mov %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_start

/* Refuses to continue on a processor that cannot do this, rather than
   faulting somewhere less explicable a few instructions later. */
check_long_mode:
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode
    mov $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx
    jz no_long_mode
    ret

no_long_mode:
    /* Say so on the screen the BIOS left behind, since there is no console
       yet and no way to make one. */
    mov $0xB8000, %edi
    mov $msg_no64, %esi
1:  lodsb
    test %al, %al
    jz 2f
    mov $0x4F, %ah
    mov %ax, (%edi)
    add $2, %edi
    jmp 1b
2:  cli
    hlt
    jmp 2b

/* Identity maps the first 4 GiB using 2 MiB pages, which needs one table at
   each of the top two levels and four at the third. */
build_tables:
    /* Clear everything first: a stale entry here faults in a way that is
       very hard to read. */
    mov $pml4, %edi
    xor %eax, %eax
    mov $(4096 * 6 / 4), %ecx
    rep stosl

    /* pml4[0] -> pdpt */
    mov $pdpt, %eax
    or $0x3, %eax                    /* present, writable */
    mov %eax, pml4

    /* pdpt[0..3] -> pd[0..3] */
    mov $pd, %eax
    or $0x3, %eax
    mov $pdpt, %edi
    mov $4, %ecx
1:  mov %eax, (%edi)
    add $4096, %eax
    add $8, %edi
    loop 1b

    /* Every page directory entry maps 2 MiB, flat, from zero upward. */
    mov $pd, %edi
    xor %eax, %eax                   /* physical address, low half */
    xor %edx, %edx                   /* high half, always zero below 4 GiB */
    mov $(512 * 4), %ecx
2:  mov %eax, (%edi)
    orl $0x83, (%edi)                /* present, writable, 2 MiB page */
    mov %edx, 4(%edi)
    add $0x200000, %eax
    adc $0, %edx
    add $8, %edi
    loop 2b
    ret

.code64
long_start:
    /* Long mode ignores most of what the data segments say, but they still
       have to be something valid. */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    mov $stack_top, %rsp
    xor %rbp, %rbp

    cmpl    $0, from_handoff(%rip)
    jne     1f

    /* A multiboot loader: build a handoff out of what it left, then go
       through the same door as everything else. */
    mov mb_magic(%rip), %edi
    mov mb_info(%rip), %esi
    call kmain_multiboot
    jmp 2f

1:  /* One of ours: it built the handoff already. */
    mov mb_info(%rip), %edi
    call kmain

2:  cli
    hlt
    jmp 2b

/* Where uefi/loader.c arrives: already in long mode, already paged, with a
   pointer to the handoff in rdi. */
.global _start64
.type _start64, @function
_start64:
    cli
    mov $stack_top, %rsp
    xor %rbp, %rbp
    call kmain
1:  cli
    hlt
    jmp 1b

.section .data
mb_magic:     .long 0
mb_info:      .long 0
from_handoff: .long 0
msg_no64: .asciz "zelr needs a 64-bit processor"
