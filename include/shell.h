#pragma once
void shell_task(void);

/* Kept at the console: no desktop, whatever the settings file says. For a
   machine being driven down a serial line by something that wants a shell,
   which is every test in this project and the reason this exists. */
void shell_console_only(void);
