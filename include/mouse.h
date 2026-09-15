#pragma once
#include "types.h"

bool mouse_init(void);
bool mouse_present(void);
i32  mouse_x(void);
i32  mouse_y(void);
u8   mouse_buttons(void);
u32  mouse_moves(void);
void mouse_hide(void);
void mouse_set_autodraw(bool on);

/* Movement from a pointer that is not the PS/2 one. dy counts the way a PS/2
   mouse counts it, upward, which is the opposite of the way the screen does
   and the opposite of the way USB reports it. */
void mouse_inject(i32 dx, i32 dy, u8 btns);
void mouse_show(void);
