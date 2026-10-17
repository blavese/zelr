#pragma once
#include "wm.h"

void app_about(void);
void app_about_forget(window_t *w);
window_t *app_about_window(void);     /* open, or 0; for the self test */
