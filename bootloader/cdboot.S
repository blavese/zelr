/* A bootloader, for booting zelr from a CD or a USB stick.
 *
 * Until now zelr has always been handed to QEMU with -kernel, which means
 * QEMU did the bootloader's job: it parsed the ELF, put the segments where
 * they belong, built a multiboot information structure and jumped to the
 * entry point. On a real machine nothing does that. This does.
 *
 * The BIOS loads this at 0x7C00 in 16-bit real mode with the boot drive in
 * DL, and from there it has to:
 *
 *   1. turn on the A20 gate, or every address above 1 MiB wraps
 *   2. ask the firmware for the memory map, because the kernel needs to be
 *      told what RAM exists and there is no other way to find out
 *   3. read the kernel off the disc, which can only be done in real mode,
 *      into a buffer below 1 MiB, which is all real mode can address
 *   4. copy each buffer up to 1 MiB, which real mode cannot do at all, so
 *      it is done in unreal mode: enter protected mode, load a segment with
 *      a 4 GiB limit, leave protected mode again, and keep the limit
 *   5. build the multiboot structure the kernel expects
 *   6. switch to protected mode for real and jump
 *
 * The four values the image builder patches sit right after the signature
 * below, so tools/mkiso.py can find them without parsing anything.
 *
 * The first 512 bytes have to stand on their own. The boot catalog asks the
 * firmware for four 512 byte sectors, and most firmware obliges, but some
 * loads one and leaves the rest of this file as whatever was in memory
 * already. Everything past 0x7E00 here is the disk packet, the descriptor
 * table and every string, so on such a machine lgdt reads garbage and the
 * jump lands nowhere: the failure looks like the disc is corrupt rather than
 * like a short read. So the code below 0x7E00 loads the whole file itself
 * before using any of it, and then never runs again.
 */
.code16
.section .text
.globl _start

_start:
    jmp     stage1
    .byte   0x90

    /* Patched by tools/mkiso.py. */
    .ascii  "ZELRBOT1"
patch_lba:      .long 0          /* first 2048 byte sector of the payload */
patch_bytes:    .long 0          /* how many bytes of payload there are */
patch_entry:    .long 0          /* where to jump once it is in memory */
patch_load:     .long 0x100000   /* where the payload belongs */
patch_self:     .long 0          /* the sector this file itself came from */

/* Where things go while we work.
 *
 * The handoff structure is what the kernel is actually given, and it is the
 * same one the UEFI loader builds: the kernel has one entry contract and does
 * not find out which loader started it. It is too big to sit below 0x7C00, so
 * it goes at 0x70000, which real mode reaches through a segment.
 *
 * The offsets are include/handoff.h laid out by hand, because assembly cannot
 * include a C header. If that structure changes, these change with it. */
.set HANDOFF_SEG,      0x7000     /* 0x70000 */
.set H_MAGIC,          0
.set H_FB_BASE,        8
.set H_KERNEL_BASE,    32
.set H_KERNEL_SIZE,    40
.set H_RSDP,           48
.set H_REGION_COUNT,   56
.set H_REGIONS,        64
.set H_LOADER,         64 + 128 * 24
.set REGION_SIZE,      24
.set MAX_REGIONS,      128

.set BUFFER_SEG,   0x1000        /* 0x10000, the disk read buffer */
/* The boot drive, parked outside this file. Stage one reads the whole file
   over itself, so anything it needs afterwards cannot be stored inside it:
   the read would replace it with the value the file has on disc, which for
   the drive number is zero, and zero is a different device. */
.set DRIVE_STASH,  0x7000
/* The read packet for stage one's own reload, and somewhere to put what
   comes back. Neither can live inside this sector: it is about to be
   replaced by the very read being described, and there has to be something
   left running to check the result. Spare low memory, below the stack and
   clear of everything stage two uses. */
.set S1_DAP,       0x7010
.set S1_SCRATCH,   0x9000
.set S1_SIG,       0x31524C5A        /* "ZLR1" */
.set CHUNK_SECTORS, 16           /* 16 x 2048 = 32 KiB per read */

/* --- stage one, which must fit below 0x7E00 ----------------------------- */

stage1:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $0x7C00, %sp
    movb    %dl, s1_drive
    movw    $DRIVE_STASH, %bx
    movb    %dl, (%bx)
    sti

    /* How big a sector is on whatever we booted from. A disc says 2048 and a
       stick says 512, and the read below counts in those. */
    movw    $2048, s1_secsize
    movw    $s1_params, %si
    movw    $0x1E, (%si)
    movb    $0x48, %ah
    int     $0x13
    jc      1f
    movw    24(%si), %ax
    testw   %ax, %ax
    jz      1f
    cmpw    $2048, %ax
    ja      1f
    movw    %ax, s1_secsize

1:  /* The sector size decides both how many to ask for and where the
       loader is, so getting it wrong reads the wrong part of the disc.
       Firmware that misreports it would hand back something that is not
       this loader at all, and jumping into that is a dead machine with
       nothing on screen to say why. So: read somewhere else, check what
       came back, and only then move it into place.

       Three attempts. What the firmware said, then the two it could
       actually be. */
    movw    s1_secsize, %ax
    movw    %ax, s1_sizes
    movw    $0, s1_try

try_read:
    movw    s1_try, %bx
    addw    %bx, %bx
    movw    s1_sizes(%bx), %ax
    testw   %ax, %ax
    jz      next_try                 /* nothing sensible reported */
    movw    %ax, s1_secsize

    /* One 2048 byte block covers this whole file, however many device
       sectors that turns out to be. */
    movw    $2048, %ax
    xorw    %dx, %dx
    divw    s1_secsize
    testw   %ax, %ax
    jz      next_try
    movw    %ax, %cx                 /* sectors to read */
    movzwl  %ax, %ebx
    movl    patch_self, %eax
    mull    %ebx                     /* eax = where, in device sectors */

    /* Built out here rather than in the image, for the reason at S1_DAP. */
    movw    $S1_DAP, %si
    movw    $0x0010, (%si)           /* packet is 16 bytes, reserved 0 */
    movw    %cx, 2(%si)
    movw    $S1_SCRATCH, 4(%si)      /* somewhere it can be looked at */
    movw    $0, 6(%si)
    movl    %eax, 8(%si)
    movl    $0, 12(%si)

    movb    s1_drive, %dl
    movb    $0x42, %ah
    int     $0x13
    jc      next_try

    /* Past the 512 bytes firmware is obliged to load, so finding it proves
       the rest of the loader really did arrive. */
    cmpl    $S1_SIG, S1_SCRATCH + (s1_signature - _start)
    je      s1_loaded

next_try:
    incw    s1_try
    cmpw    $3, s1_try
    jb      try_read
    jmp     s1_failed

s1_loaded:
    /* Move it into place. The first 512 bytes are what is executing right
       now and are byte for byte the same, so overwriting them mid-copy
       changes nothing. */
    cld
    movw    $S1_SCRATCH, %si
    movw    $0x7C00, %di
    movw    $1024, %cx
    rep     movsw

    movw    $DRIVE_STASH, %bx
    movb    (%bx), %dl
    jmp     real_start

s1_failed:
    movw    $s1_msg, %si
2:  lodsb
    testb   %al, %al
    jz      3f
    movb    $0x0E, %ah
    movw    $0x0007, %bx
    int     $0x10
    jmp     2b
3:  hlt
    jmp     3b

.align 4
s1_drive:   .byte 0
s1_secsize: .word 2048
s1_try:     .word 0
/* What the firmware said, then the only two a boot device actually uses. */
s1_sizes:   .word 0, 2048, 512
s1_msg:     .asciz "zelr: cannot read the boot device\r\n"

.align 4
s1_params:  .space 0x1E

/* --- stage two ---------------------------------------------------------- */

real_start:
    movb    %dl, boot_drive

    movw    $msg_boot, %si
    call    print

    /* Each step names itself before it runs. On firmware that stops
       partway there is nothing else to go on: the screen is all there is,
       and the last word on it is the step that did not finish. */
    movw    $msg_a20, %si
    call    print
    call    enable_a20

    movw    $msg_geo, %si
    call    print
    call    probe_geometry
    call    zero_handoff

    movw    $msg_mem, %si
    call    print
    call    read_memory_map

    movw    $msg_unreal, %si
    call    print
    call    enter_unreal

    movw    $msg_load, %si
    call    print
    call    load_payload

    movw    $msg_go, %si
    call    print

    cli
    lgdt    gdt_desc
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmpl   $0x08, $protected

/* --- real mode helpers -------------------------------------------------- */

/* Prints the null terminated string at DS:SI through the BIOS. This is the
   only output there is on a machine that has not started zelr yet, so every
   failure below ends in one of these. */
print:
    pushw   %ax
    pushw   %bx
    movb    $0x0E, %ah
    movw    $0x0007, %bx
1:  lodsb
    testb   %al, %al
    jz      2f
    int     $0x10
    jmp     1b
2:  popw    %bx
    popw    %ax
    ret

die:
    call    print
    movw    $msg_halt, %si
    call    print
1:  hlt
    jmp     1b

/* The A20 gate starts closed on a PC for compatibility with software from
   1982. Try the fast path through the system control port first, then the
   keyboard controller, which is what older machines need. */
/* Opening the A20 gate, and refusing to go on without it.
 *
 * With the gate shut, every address above 1 MiB wraps back to zero. The next
 * thing this loader does is copy the kernel to 1 MiB, so a shut gate means
 * copying it over the interrupt table and the firmware's own data instead.
 * The machine does not survive that, and what it looks like from outside is a
 * triple fault with nothing on screen to explain it.
 *
 * So there are four ways to open it, each one tested rather than assumed, and
 * a refusal to continue if none of them worked. The previous version tried
 * two and checked after the first, which is the same as not checking. */

/* 1 in AX when the gate is open, 0 when addresses still wrap. Compares a byte
   at 0x0000:0x0500 with the one 1 MiB above it at 0xFFFF:0x0510, and puts
   both back afterwards. */
a20_test:
    pushw   %ds
    pushw   %es
    pushw   %bx
    pushw   %si
    pushw   %di

    xorw    %ax, %ax
    movw    %ax, %ds
    movw    $0xFFFF, %ax
    movw    %ax, %es
    movw    $0x0500, %si
    movw    $0x0510, %di

    movb    (%si), %al
    pushw   %ax
    movb    %es:(%di), %al
    pushw   %ax

    movb    $0x00, (%si)
    movb    $0xFF, %es:(%di)
    movb    (%si), %al
    cmpb    $0xFF, %al           /* still equal means the address wrapped */

    /* Put the two bytes back. None of this disturbs the flags. */
    popw    %bx
    movb    %bl, %es:(%di)
    popw    %bx
    movb    %bl, (%si)

    movw    $1, %ax
    jne     1f
    xorw    %ax, %ax
1:  popw    %di
    popw    %si
    popw    %bx
    popw    %es
    popw    %ds
    ret

/* The keyboard controller, which is where the gate lived before there was a
   faster way. The waits are bounded: a controller that never answers should
   cost a moment and a fallback, not the whole machine. */
kbc_a20:
    pushw   %ax
    pushw   %cx

    call    kbc_wait_in
    movb    $0xAD, %al           /* disable the keyboard while we work */
    outb    %al, $0x64

    call    kbc_wait_in
    movb    $0xD0, %al           /* read the output port */
    outb    %al, $0x64
    call    kbc_wait_out
    inb     $0x60, %al
    pushw   %ax

    call    kbc_wait_in
    movb    $0xD1, %al           /* write it back with the gate bit set */
    outb    %al, $0x64
    call    kbc_wait_in
    popw    %ax
    orb     $0x02, %al
    outb    %al, $0x60

    call    kbc_wait_in
    movb    $0xAE, %al           /* and enable the keyboard again */
    outb    %al, $0x64
    call    kbc_wait_in

    popw    %cx
    popw    %ax
    ret

kbc_wait_in:
    pushw   %cx
    movw    $0xFFFF, %cx
1:  inb     $0x64, %al
    testb   $0x02, %al
    jz      2f
    loop    1b
2:  popw    %cx
    ret

kbc_wait_out:
    pushw   %cx
    movw    $0xFFFF, %cx
1:  inb     $0x64, %al
    testb   $0x01, %al
    jnz     2f
    loop    1b
2:  popw    %cx
    ret

enable_a20:
    call    a20_test
    testw   %ax, %ax
    jnz     a20_done             /* the firmware already opened it */

    /* Ask the firmware, which is the one allowed to be clever about how. */
    movw    $0x2401, %ax
    int     $0x15
    call    a20_test
    testw   %ax, %ax
    jnz     a20_done

    /* The fast gate on the system control port. */
    inb     $0x92, %al
    testb   $0x02, %al
    jnz     1f
    orb     $0x02, %al
    andb    $0xFE, %al           /* bit 0 would reset the machine */
    outb    %al, $0x92
1:  call    a20_test
    testw   %ax, %ax
    jnz     a20_done

    call    kbc_a20
    call    a20_test
    testw   %ax, %ax
    jnz     a20_done

    /* Going on from here would copy the kernel over the interrupt table. */
    movw    $msg_a20fail, %si
    jmp     die

a20_done:
    ret

/* A CD reports 2048 byte sectors and a USB stick reports 512, and INT 13h
   counts in whatever the device uses. The image builder works in 2048 byte
   units because that is what an ISO is made of, so everything here is scaled
   by whatever the firmware says once, at the start. */
probe_geometry:
    movw    $2048, sector_size
    movw    $1, lba_scale

    movw    $params, %si
    movw    $0x1E, (%si)         /* buffer size the call expects */
    movb    boot_drive, %dl
    movb    $0x48, %ah
    int     $0x13
    jc      1f

    movw    24(%si), %ax         /* bytes per sector */
    testw   %ax, %ax
    jz      1f
    cmpw    $2048, %ax
    ja      1f                   /* something unexpected: keep the default */
    movw    %ax, sector_size

    /* lba_scale converts an ISO sector number into a device one. */
    movw    $2048, %ax
    xorw    %dx, %dx
    divw    sector_size
    movw    %ax, lba_scale

1:  /* Sectors per 32 KiB chunk, which is what each read asks for. */
    movw    $32768, %ax
    xorw    %dx, %dx
    divw    sector_size
    movw    %ax, chunk_sectors
    ret

/* Asks the firmware what memory exists and writes it straight into the
   handoff.
 *
 * The firmware's answer and the kernel's structure happen to agree for the
 * first twenty bytes: an eight byte base, an eight byte length and a four
 * byte kind. So the call writes into the slot directly and only the padding
 * and the kind need touching afterwards. */
read_memory_map:
    pushw   %es
    movw    $HANDOFF_SEG, %ax
    movw    %ax, %es

    movw    $H_REGIONS, %di
    xorl    %ebx, %ebx
    xorl    %ebp, %ebp           /* how many regions written */

1:  movl    $20, %ecx
    movl    $0x0000E820, %eax
    movl    $0x534D4150, %edx    /* 'SMAP' */
    int     $0x15
    jc      2f
    cmpl    $0x534D4150, %eax
    jne     2f

    /* The firmware's kinds are not ours. Usable and ACPI-reclaimable happen
       to share a number; everything else is something the kernel must not
       hand out. */
    movl    %es:16(%di), %eax
    cmpl    $1, %eax
    je      4f
    cmpl    $3, %eax
    je      4f
    movl    $2, %es:16(%di)
4:  movl    $0, %es:20(%di)      /* the padding, which nothing sets for us */

    addw    $REGION_SIZE, %di
    incl    %ebp

    testl   %ebx, %ebx
    jz      3f
    cmpl    $MAX_REGIONS, %ebp
    jae     3f
    jmp     1b

2:  testl   %ebp, %ebp
    jnz     3f
    popw    %es
    movw    $msg_nomem, %si
    jmp     die

3:  movl    %ebp, %es:H_REGION_COUNT
    movl    $0, %es:H_REGION_COUNT + 4
    popw    %es
    ret

/* Enters protected mode just long enough to give FS a 4 GiB limit, then
   leaves again. The limit stays in the hidden part of the segment register,
   so real mode code can reach the whole address space through FS. This is
   the only way to write above 1 MiB without giving up the BIOS. */
enter_unreal:
    cli
    pushw   %ds
    lgdt    gdt_desc
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    jmp     1f
1:  movw    $0x10, %bx
    movw    %bx, %fs
    movl    %cr0, %eax
    andl    $0xFFFFFFFE, %eax
    movl    %eax, %cr0
    jmp     2f
2:  popw    %ds
    sti
    ret

/* Reads the payload off the boot device 32 KiB at a time and copies each
   chunk to its real home above 1 MiB. */
load_payload:
    movl    patch_lba, %eax
    movl    %eax, cur_lba
    movl    patch_load, %eax
    movl    %eax, cur_dest
    movl    patch_bytes, %eax
    movl    %eax, remaining

1:  cmpl    $0, remaining
    jbe     9f

    /* One extended read into the buffer below 1 MiB, in whatever sector
       size this device happens to use. The last chunk asks for only as many
       sectors as are left: reading past the end of the image is what a
       drive refuses, and it looks exactly like a broken disc. */
    movw    chunk_sectors, %ax
    movl    remaining, %ecx
    cmpl    $CHUNK_SECTORS * 2048, %ecx
    jae     2f
    movw    %cx, %ax
    addw    sector_size, %ax
    decw    %ax
    xorw    %dx, %dx
    divw    sector_size
2:  movw    $dap, %si
    movw    %ax, dap_count
    movl    cur_lba, %eax
    movzwl  lba_scale, %ecx
    mull    %ecx
    movl    %eax, dap_lba
    movb    boot_drive, %dl
    movb    $0x42, %ah
    int     $0x13
    jc      read_failed

    /* Copy it up. ECX counts dwords; a chunk is 32 KiB so it fits in CX. */
    movl    $BUFFER_SEG * 16, %esi
    movl    cur_dest, %edi
    movl    remaining, %ecx
    cmpl    $CHUNK_SECTORS * 2048, %ecx
    jbe     2f
    movl    $CHUNK_SECTORS * 2048, %ecx
2:  addl    $3, %ecx
    shrl    $2, %ecx

3:  movl    %fs:(%esi), %eax
    movl    %eax, %fs:(%edi)
    addl    $4, %esi
    addl    $4, %edi
    decl    %ecx
    jnz     3b

    /* On to the next chunk. cur_lba stays in ISO sectors. */
    addl    $16, cur_lba
    movl    $CHUNK_SECTORS * 2048, %eax
    addl    %eax, cur_dest
    subl    %eax, remaining
    jc      9f                   /* the last chunk was short */

    movw    $msg_dot, %si
    call    print
    jmp     1b

9:  movw    $msg_nl, %si
    call    print
    ret

read_failed:
    movw    $msg_readfail, %si
    jmp     die

/* Clears the handoff and fills in everything except the memory map, which
   read_memory_map writes directly.
 *
 * The framebuffer is left at zero on purpose: this machine has a BIOS, so the
 * kernel can set a mode itself through the VBE ports, and it will pick one
   rather than being told. The ACPI pointer is left at zero too, because on a
   BIOS machine the kernel's own search of low memory finds it. */
zero_handoff:
    pushw   %es
    movw    $HANDOFF_SEG, %ax
    movw    %ax, %es

    xorw    %di, %di
    movw    $(H_LOADER + 160), %cx    /* the whole structure, in bytes */
    xorb    %al, %al
1:  movb    %al, %es:(%di)
    incw    %di
    decw    %cx
    jnz     1b

    /* The magic the kernel checks before it trusts any of this. */
    /* HANDOFF_MAGIC, low half first, which is how it lands in memory. */
    movl    $0x36344846, %es:H_MAGIC
    movl    $0x5A454C52, %es:H_MAGIC + 4

    movl    $0x00100000, %es:H_KERNEL_BASE
    movl    $0, %es:H_KERNEL_BASE + 4
    movl    patch_bytes, %eax
    movl    %eax, %es:H_KERNEL_SIZE
    movl    $0, %es:H_KERNEL_SIZE + 4

    /* Which loader this was, for the boot banner. */
    movw    $H_LOADER, %di
    movw    $loader_name, %si
2:  lodsb
    movb    %al, %es:(%di)
    incw    %di
    testb   %al, %al
    jnz     2b

    popw    %es
    ret

/* --- protected mode ----------------------------------------------------- */

.code32
protected:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movl    $0x7C00, %esp

    /* The kernel states its entry points in the first bytes of its own
       image rather than being loaded at a fixed one: the second is for a
       loader still in 32-bit protected mode, which is what this is. */
    movl    $0x00100000, %esi
    movl    16(%esi), %ecx               /* entry32 */

    movl    $(HANDOFF_SEG * 16), %ebx    /* the handoff, in ebx */
    jmp     *%ecx

/* --- data --------------------------------------------------------------- */

.code16
boot_drive:  .byte 0
.align 4
mmap_bytes:  .long 0
sector_size:   .word 2048
lba_scale:     .word 1
chunk_sectors: .word CHUNK_SECTORS
cur_lba:     .long 0
cur_dest:    .long 0
remaining:   .long 0

.align 4
dap:
    .byte   16                   /* packet size */
    .byte   0
dap_count:
    .word   0
dap_offset:
    .word   0
dap_segment:
    .word   BUFFER_SEG
dap_lba:
    .long   0
    .long   0

/* INT 13h AH=48h writes its answer here. */
.align 4
params:
    .space  0x1E

.align 8
gdt:
    .quad   0
    /* code32: base 0, limit 4 GiB, present, ring 0, execute/read */
    .word   0xFFFF, 0x0000
    .byte   0x00, 0x9A, 0xCF, 0x00
    /* data32: base 0, limit 4 GiB, present, ring 0, read/write */
    .word   0xFFFF, 0x0000
    .byte   0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_desc:
    .word   gdt_end - gdt - 1
    .long   gdt

loader_name:  .asciz "bios"
msg_boot:     .asciz "zelr bootloader\r\n"
msg_a20:      .asciz "a20 "
msg_geo:      .asciz "geometry "
msg_mem:      .asciz "memory "
msg_unreal:   .asciz "unreal "
msg_load:     .asciz "loading "
msg_dot:      .asciz "."
msg_nl:       .asciz "\r\n"
msg_go:       .asciz "starting kernel\r\n"
msg_halt:     .asciz "\r\nhalted.\r\n"
msg_nomem:    .asciz "the firmware would not describe memory"
msg_readfail: .asciz "could not read the boot device"
msg_a20fail:  .asciz "the a20 gate will not open"

/* Sits past the 512 bytes firmware is obliged to load, so stage one finding
   it is proof the whole file arrived rather than just the first sector. Its
   offset is whatever the assembler puts it at; stage one refers to it by
   name, so nothing has to agree on a number. bootloader/build.sh checks it
   landed where it has to, because the assembler cannot: the difference of
   two symbols is not known until they are linked. */
.align 4
.globl s1_signature
s1_signature:
    .long 0x31524C5A
