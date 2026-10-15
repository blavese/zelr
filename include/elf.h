#pragma once
#include "types.h"

#define ELF_OK            0
#define ELF_ERR_SHORT    -1
#define ELF_ERR_MAGIC    -2
#define ELF_ERR_CLASS    -3
#define ELF_ERR_TYPE     -4
#define ELF_ERR_RANGE    -5
#define ELF_ERR_OVERFLOW -6
#define ELF_ERR_MEMORY   -7

/* Loads every PT_LOAD segment into the given address space. */
int elf_load(u64 dir, const u8 *image, u64 size, u64 *entry_out);

/* The pages of the image elf_load last loaded that hold nothing but zeros,
   left unmapped for the fault handler to give out when they are touched:
   [*lo, *hi), both 0 for none. The caller gives them to the task. */
void elf_zero_range(u64 *lo, u64 *hi);
const char *elf_error(int code);
