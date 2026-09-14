/* The master boot record, for when the image is written to a USB stick.
 *
 * A BIOS booting from a disc reads the El Torito catalog and loads the
 * bootloader itself. A BIOS booting from a stick does no such thing: it
 * loads the first 512 bytes of the device and jumps to them. That is this.
 *
 * Its only job is to fetch the real bootloader, which is a few sectors
 * further into the same image, and hand over. The complication is that both
 * want to live at 0x7C00, so this moves itself out of the way to 0x0600
 * first, which is the same trick every MBR since 1983 has used.
 *
 * Everything after byte 446 belongs to the partition table, so this has to
 * fit in 446 bytes. It currently uses about a third of that.
 */
.code16
.section .text
.globl _start

.set RELOC,  0x0600
.set LOADER, 0x7C00

_start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $LOADER, %sp
    cld

    /* Move these 512 bytes to 0x0600 and continue there, so the bootloader
       can be read into 0x7C00 without overwriting the code doing the
       reading. */
    movw    $LOADER, %si
    movw    $RELOC, %di
    movw    $256, %cx
    rep movsw
    ljmp    $0, $RELOC + (moved - _start)

moved:
    sti
    movb    %dl, drive - _start + RELOC

    movw    $msg - _start + RELOC, %si
    call    print

    /* One extended read brings the bootloader in. A stick reports 512 byte
       sectors, which is what the values below are in. */
    movw    $dap - _start + RELOC, %si
    movb    $0x42, %ah
    movb    drive - _start + RELOC, %dl
    int     $0x13
    jc      failed

    movb    drive - _start + RELOC, %dl
    ljmp    $0, $LOADER

failed:
    movw    $msg_fail - _start + RELOC, %si
    call    print
1:  hlt
    jmp     1b

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

drive:  .byte 0

.align 4
/* Patched by tools/mkiso.py: how many 512 byte sectors of bootloader there
   are, and where they start. */
dap:
    .byte   16
    .byte   0
dap_count:
    .word   4
dap_offset:
    .word   LOADER
dap_segment:
    .word   0
dap_lba:
    .long   92
    .long   0

msg:      .asciz "zelr\r\n"
msg_fail: .asciz "no bootloader\r\n"
