#pragma once
#include "types.h"

/* The 8042, which the keyboard and the mouse both arrive through.
 *
 * Owned in one place because there is one controller and one configuration
 * byte. Two drivers each setting the bit they care about is how the other
 * one's bit gets lost, and a lost bit here is a machine nobody can type on.
 *
 * It is also the one piece of hardware in this kernel that cannot be trusted
 * to be in a known state at boot. The firmware used it, and what it leaves
 * behind is different on a cold start, on a warm start, and on the same
 * machine configured differently. So nothing here reads a setting and hopes;
 * every bit that matters is written. */

bool ps2_present(void);

/* Drains whatever the firmware left, configures the controller, enables both
   ports, and tells the keyboard to start sending. Safe to call on a machine
   that has no 8042 at all, which is most laptops made this decade. */
void ps2_init(void);

/* Takes every byte that is waiting and hands each to whichever driver it was
   for. Called from both interrupt handlers and from the timer tick, so a byte
   that arrives when nobody is listening still gets read.
 *
   Returns how many bytes it took. */
u32 ps2_poll(void);

/* The same, from the timer tick, counting anything it finds as a byte no
   interrupt arrived for. */
void ps2_poll_from_timer(void);

/* Talking to the controller. Bounded: a port nobody answers reads 0xFF
   forever and a driver that waits for it never comes back. */
void ps2_command(u8 cmd);
void ps2_write_data(u8 byte);
bool ps2_read(u8 *out);

/* What it looked like when the kernel found it, what it looks like now, and
   how much of the input had to be fetched by the timer rather than delivered.
   In the boot log because these three say why a keyboard is not working far
   faster than anything else. */
u8  ps2_first_status(void);
u8  ps2_first_config(void);
u8  ps2_config(void);
u32 ps2_rescued(void);
