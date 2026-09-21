/* The kernel is linked to run at 16 MiB physically. Both loaders put it
   there and neither relocates it, so this address is the one that matters
   and everything that needs it reads it from here: build.sh and
   tools/mkiso.py through tools/loadaddr.py, and the two loaders through
   tools/check_loader.py, which refuses a build where they disagree.

   It was 1 MiB, which is where a 32-bit kernel goes because it is the first
   address above the memory the BIOS era carved up. It stopped working the
   moment the kernel grew past seven megabytes: UEFI firmware keeps its ACPI
   tables a little way up -- the one tested against puts its NVS at eight --
   and a kernel linked at one megabyte and nine megabytes long needs the
   range with those tables in the middle of it. That range is not the
   firmware's to give and not the loader's to take, so the UEFI paths simply
   stopped booting, and stayed that way for two releases because the loader
   reported a status code and nothing else.

   Sixteen megabytes clears it with room underneath, and sits inside the
   64 MiB kernel/paging.c maps a page at a time, so the kernel image is
   still described by page tables the kernel can change one at a time.

   What it costs is fifteen megabytes below the kernel that now hold
   nothing, and a floor on memory that moves up by the same amount, because
   the frame bitmap and the heap both follow the image. Measured rather than
   worked out: it boots and passes its own 552 checks on a machine with
   forty megabytes. The desktop wants more than that, for the compositor's
   two full-screen buffers, and always did.

   .kheader comes first so a loader can read the entry point out of the first
   sixteen bytes of the flat image, and .multiboot must stay inside the first
   8 KiB for a multiboot loader to find it. */
ENTRY(_start)
SECTIONS {
    . = 16M;
    __kernel_start = .;
    .text : ALIGN(4K) { *(.kheader) *(.multiboot) *(.text*) }
    .rodata : ALIGN(4K) { *(.rodata*) }
    .data : ALIGN(4K) { *(.data*) }
    .bss : ALIGN(4K) { __bss_start = .; *(COMMON) *(.bss*) __bss_end = .; }
    __kernel_end = .;
}
