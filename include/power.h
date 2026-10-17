#pragma once
#include "types.h"

/* Turning the machine off, which is not an instruction but a request to the
   chipset for sleep state five. What has to be written to make that request
   is two numbers the firmware chose and kept in its bytecode. */

bool power_can_off(void);

/* Why it cannot, when it cannot, for the boot log and the shell. */
const char *power_describe(void);

/* Does not return, on a machine where it works. Returns false on one where
   the firmware wanted something else, rather than hanging. */
bool power_off(void);

/* For the self test: the firmware takes no notice of power_off, which then
   only waits and says it could not. */
void power_test_ignored(bool on);

/* Does not return either. Three ways are tried, because a machine without a
   keyboard controller does not answer the usual one. */
void power_reboot(void);
