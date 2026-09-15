#pragma once
#include "types.h"

/* Keys that are not characters.
 *
 * A PS/2 keyboard reports these as a 0xE0 prefix followed by an ordinary
 * scancode, which is how the same byte means "P" and "page down" depending
 * on what came before it. They are given values above anything a byte can
 * hold so that one integer can carry either a character or a key. */
#define KEY_UP        0x100
#define KEY_DOWN      0x101
#define KEY_LEFT      0x102
#define KEY_RIGHT     0x103
#define KEY_HOME      0x104
#define KEY_END       0x105
#define KEY_PAGE_UP   0x106
#define KEY_PAGE_DOWN 0x107
#define KEY_DELETE    0x108
#define KEY_INSERT    0x109
#define KEY_F1        0x110      /* F1..F12 run consecutively */

/* Which modifiers were held at the moment the key was pressed.
 *
 * These travel with the key rather than being asked for afterwards. A key
 * sits in the buffer until somebody reads it, and by then the modifier has
 * usually been let go: measured on a chord sent as fast as a machine can
 * send one, every arrow key arrived with alt already false, so a shortcut
 * built on reading the flag later worked only by luck. */
#define KEY_MOD_ALT   0x10000
#define KEY_MOD_CTRL  0x20000
#define KEY_MOD_SHIFT 0x40000
#define KEY_MODS      0x70000

/* The key itself, with the modifiers taken off. */
#define KEY_CODE(k)   ((k) & 0xFFFF)

#define KEY_IS_SPECIAL(k) (KEY_CODE(k) >= 0x100)

void keyboard_init(void);

/* A key from a keyboard that is not the PS/2 one.
 *
 * A USB keyboard reports which modifiers were held in the same report as the
 * key, so the key arrives already stamped rather than being stamped here
 * from state this file keeps. It goes into the same ring buffer, which is
 * what lets everything above this not care which keyboard was typed on. */
void keyboard_inject(int key);

/* And what it is holding down now, for kbd_alt and the two beside it. */
void keyboard_set_mods(bool alt, bool ctrl, bool shift);
bool kbd_has_char(void);
char kbd_getchar(void);          /* blocking */
int  kbd_trygetchar(void);       /* -1 when empty */

/* Whether a modifier is held at this instant. Right for asking about the
   state of the keyboard now; wrong for asking what was held when a key that
   is being read was pressed, which is what KEY_MOD_* on the key says. */
bool kbd_alt(void);
bool kbd_ctrl(void);
bool kbd_shift(void);
