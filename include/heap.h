#pragma once
#include "types.h"
void  heap_init(u64 start, u64 size);
void *kmalloc(size_t n);
void *kcalloc(size_t n);
void  kfree(void *p);
u32   heap_used(void);
u32   heap_total(void);

/* Walks every block: each inside the heap and next to the one after it in
   memory, the links agreeing both ways, no two free blocks side by side, and
   the sizes adding up to the heap and to what is counted as used. */
bool  heap_check(void);

/* For the self test: called inside kmalloc between choosing a block and
   claiming it, the moment an interrupt used to be able to get in. */
extern void (*heap_test_probe)(void);
