/* ELF64 loader.
 *
 * Everything here is reading a file that the kernel did not produce, so every
 * field is checked before it is used. A loader that trusts its input is a
 * loader that can be handed a program that maps over the kernel. */
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "user.h"

#define ET_EXEC    2
#define EM_X86_64  62
#define PT_LOAD    1

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct {
    u8  ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed)) elf64_hdr_t;

/* The 64-bit program header is not the 32-bit one widened: flags moves ahead
   of the offsets, which is the sort of difference that silently loads the
   wrong bytes if it is missed. */
typedef struct {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed)) elf64_phdr_t;

/* User space starts above the kernel's identity mapped region and stops
   below where user stacks live. */
/* A program may only ask to be placed inside user space. Anything else is
   refused rather than mapped, which is what stops an image asking to be
   loaded over the kernel. */
#define USER_LOAD_MIN USER_SPACE_BASE
/* And below all of the room the stack may grow into (user.h), taken from
   there: it was a number of its own, a megabyte under the stack's top,
   and the stack's room grew to eight under it. */
#define USER_LOAD_MAX (USER_STACK_TOP - USER_STACK_MAX)

const char *elf_error(int code) {
    switch (code) {
        case ELF_OK:            return "ok";
        case ELF_ERR_SHORT:     return "file is too small to be an executable";
        case ELF_ERR_MAGIC:     return "not an ELF file";
        case ELF_ERR_CLASS:     return "not a 64-bit little-endian ELF";
        case ELF_ERR_TYPE:      return "not an executable for this machine";
        case ELF_ERR_RANGE:     return "a segment lies outside user memory";
        case ELF_ERR_OVERFLOW:  return "a segment runs off the end of the file";
        case ELF_ERR_MEMORY:    return "out of memory while loading";
        default:                return "unknown error";
    }
}

/* A program's zero data -- the browser's is six megabytes of tables it
   fills as far as a page needs -- was given a page each when it started,
   touched or not, which on a machine of 140 megabytes is memory every
   program paid for whether it used it or not. The whole pages of a
   segment past its last byte from the file are left unmapped and handed
   out zeroed when first touched, as the stack's are (user.c). One range: a
   second segment with zero pages has them mapped as before. */
static u64 zero_lo, zero_hi;

void elf_zero_range(u64 *lo, u64 *hi) { *lo = zero_lo; *hi = zero_hi; }

int elf_load(u64 dir, const u8 *image, u64 size, u64 *entry_out) {
    zero_lo = zero_hi = 0;
    if (size < sizeof(elf64_hdr_t)) return ELF_ERR_SHORT;

    const elf64_hdr_t *h = (const elf64_hdr_t *)image;
    if (h->ident[0] != 0x7F || h->ident[1] != 'E' ||
        h->ident[2] != 'L'  || h->ident[3] != 'F') return ELF_ERR_MAGIC;
    if (h->ident[4] != 2 || h->ident[5] != 1) return ELF_ERR_CLASS;   /* 64-bit LE */
    if (h->type != ET_EXEC || h->machine != EM_X86_64) return ELF_ERR_TYPE;

    if (h->phoff == 0 || h->phentsize < sizeof(elf64_phdr_t)) return ELF_ERR_SHORT;
    if (h->phoff + (u64)h->phnum * h->phentsize > size) return ELF_ERR_OVERFLOW;
    if (h->entry < USER_LOAD_MIN || h->entry >= USER_LOAD_MAX) return ELF_ERR_RANGE;

    for (u32 i = 0; i < h->phnum; i++) {
        const elf64_phdr_t *p =
            (const elf64_phdr_t *)(image + h->phoff + (u64)i * h->phentsize);
        if (p->type != PT_LOAD) continue;
        if (p->memsz == 0) continue;

        if (p->filesz > p->memsz) return ELF_ERR_OVERFLOW;
        if (p->offset + p->filesz < p->offset) return ELF_ERR_OVERFLOW;
        if (p->offset + p->filesz > size) return ELF_ERR_OVERFLOW;
        if (p->vaddr < USER_LOAD_MIN) return ELF_ERR_RANGE;
        if (p->vaddr + p->memsz < p->vaddr) return ELF_ERR_RANGE;
        if (p->vaddr + p->memsz > USER_LOAD_MAX) return ELF_ERR_RANGE;

        /* Pages are the unit of mapping, so cover whatever range the segment
           touches, then place the bytes at their real offset inside it. */
        u64 start = p->vaddr & ~0xFFFull;
        u64 end   = (p->vaddr + p->memsz + PAGE_SIZE - 1) & ~0xFFFull;

        /* From the first whole page past the file's bytes, nothing but
           zeros: left for the fault handler, when this is the first such
           span. */
        u64 bytes_end = (p->vaddr + p->filesz + PAGE_SIZE - 1) & ~0xFFFull;
        if (bytes_end < start) bytes_end = start;
        if (!zero_hi && bytes_end < end) {
            zero_lo = bytes_end;
            zero_hi = end;
            end = bytes_end;
        }

        for (u64 va = start; va < end; va += PAGE_SIZE) {
            if (virt_to_phys_in(dir, va)) continue;      /* segments can share a page */
            u64 frame = pmm_alloc_frame();
            if (!frame) return ELF_ERR_MEMORY;
            memset((void *)frame, 0, PAGE_SIZE);          /* .bss arrives zeroed */
            if (!map_page_in(dir, va, frame, PTE_PRESENT | PTE_RW | PTE_USER)) {
                pmm_free_frame(frame);
                return ELF_ERR_MEMORY;
            }
        }

        for (u64 done = 0; done < p->filesz; ) {
            u64 va = p->vaddr + done;
            u64 phys = virt_to_phys_in(dir, va);
            if (!phys) return ELF_ERR_MEMORY;

            u64 room = PAGE_SIZE - (va & 0xFFF);
            u64 n = p->filesz - done;
            if (n > room) n = room;
            memcpy((void *)phys, image + p->offset + done, n);
            done += n;
        }
    }

    *entry_out = h->entry;
    return ELF_OK;
}
