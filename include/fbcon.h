#pragma once
#include "types.h"

void fbcon_init(void);
void fbcon_putc(char c);
void fbcon_clear(void);
void fbcon_set_color(u8 fg, u8 bg);
u32  fbcon_cols(void);
u32  fbcon_rows(void);

/* While the desktop has the screen the console draws nothing on it. It drew
   into the same back buffer the desktop composites into, so a program's
   printing (the browser's line for every page) was written over the
   desktop, and once the text reached the bottom the whole desktop was
   scrolled up a quarter of the screen; drawing only what changed, the
   desktop left it so. The words still go to the serial line, and the shell
   clears the console when the desktop gives the screen back. */
void fbcon_hold(bool held);
