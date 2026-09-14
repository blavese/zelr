#pragma once

/* What a bootloader hands the kernel.
 *
 * There are three ways into this kernel and they agree on exactly one thing:
 * a pointer to this structure. The UEFI loader builds it from firmware
 * services, the BIOS loader builds it from INT 15h and the VBE ports, and the
 * multiboot entry builds it from what a multiboot loader left behind. After
 * that the kernel never asks how it was started.
 *
 * It is deliberately not multiboot. Multiboot cannot describe a framebuffer
 * the firmware chose, has no room for an ACPI pointer, and is 32-bit; all
 * three matter on a machine that booted through UEFI.
 *
 * The layout is fixed: the loaders are separate programs, built separately,
 * and one of them is a PE binary built for a different ABI.
 */

#define HANDOFF_MAGIC 0x5A454C5236344846ull   /* "ZELR64HF" */

/* Kinds of memory, which is all the kernel needs to know. The firmware has
   about fifteen; they collapse to these. */
#define MEM_USABLE    1
#define MEM_RESERVED  2
#define MEM_ACPI      3    /* reclaimable once the tables have been read */

typedef struct {
    unsigned long long base;
    unsigned long long len;
    unsigned int type;
    unsigned int pad;
} mem_region_t;

#define HANDOFF_MAX_REGIONS 128

typedef struct {
    unsigned long long magic;

    /* The screen, already set up. On UEFI there is no way to set a mode
       afterwards, so whatever the loader chose is what there is. */
    unsigned long long fb_base;
    unsigned int fb_width, fb_height;
    unsigned int fb_pitch;          /* in pixels, not bytes */
    unsigned int fb_bpp;

    /* Where the kernel image was put, so it can avoid handing itself out. */
    unsigned long long kernel_base;
    unsigned long long kernel_size;

    /* The root of the ACPI tables. Zero if the firmware did not say, in
       which case the kernel falls back to searching low memory. */
    unsigned long long rsdp;

    unsigned long long region_count;
    mem_region_t regions[HANDOFF_MAX_REGIONS];

    char loader[32];                /* which one, for the boot banner */
    char cmdline[128];
} handoff_t;
