#pragma once
#include "types.h"

/* The apps kept on the taskbar, in the order they sit there.
 *
 * A pin is a program and the name to show for it, which is everything the
 * taskbar needs and everything that has to survive a reboot. They live in a
 * file of their own rather than in the theme: the settings program rewrites
 * the theme file whole, and would take the pins with it.
 *
 * The order is the list order, so moving an icon along the panel is moving
 * an entry in an array. */

#define PIN_MAX    8
#define PIN_LABEL  16
#define PIN_PATH   32
#define PIN_FILE   "/zelr.pins"

typedef struct {
    char label[PIN_LABEL];
    char path[PIN_PATH];
} pin_t;

/* Reads the file. With no file at all the list starts as the programs the
   machine ships with; with an empty one it starts empty, because somebody
   emptied it. */
void pins_init(void);

int          pins_count(void);
const pin_t *pin_at(int i);
int          pins_find(const char *path);     /* -1 when it is not pinned */

/* These two write the file, so nothing has to remember to. Moving does not:
   a drag crosses several slots and would write the disk at each one, so the
   one caller that moves things saves once when the drag ends. */
bool pins_add(const char *label, const char *path);
void pins_remove(int i);
void pins_move(int from, int to);

bool pins_save(void);

/* Re-reads the file and says whether it changed, so the panel notices the
   settings program rewriting it without either side knowing about the
   other. The same arrangement the theme has. */
bool pins_reload(void);
