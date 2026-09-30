#pragma once
#include "types.h"

/* Creates the directory layout and the files that ship with the system, once
   per disk rather than once per boot. Empties /tmp. */
void layout_init(void);

/* Everything in /tmp gone, folders and what is in them included. */
void layout_empty_tmp(void);

/* Where the shell starts. */
const char *layout_home(void);
