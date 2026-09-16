#pragma once
#include "types.h"

/* USB, as far as a keyboard and a mouse need it.
 *
 * This is the part that makes zelr usable on a real machine. A laptop built
 * in the last decade has no PS/2 controller behind its keyboard: the keys go
 * to a USB device soldered to the board, the touchpad is another one, and
 * until the firmware hands over they only work because the firmware is
 * pretending. It stops pretending the moment an operating system takes the
 * controller, which is why a kernel that does not speak USB looks like it
 * works right up until it does not.
 *
 * Only the boot protocol is implemented. Every keyboard and mouse is
 * required to support it, it is eight bytes for a keyboard and three or four
 * for a mouse, and it exists precisely so that something with no room for a
 * report descriptor parser can still be typed on. Reading the full report
 * descriptor is what a general purpose driver does; this is the other one. */

void usb_init(void);

/* Drains the controller's event ring. Called from the timer tick, so a
   keyboard works without anything else having to remember to ask. */
void usb_poll(void);

bool usb_present(void);
u32  usb_keyboards(void);
u32  usb_mice(void);
u32  usb_hubs(void);
u32  usb_disks(void);

/* Starts the task that watches for something being plugged in. Separate from
   usb_init because it needs a scheduler, and usb_init runs long before there
   is one. */
void usb_start_service(void);

/* How many reports have arrived from all of them. Zero after a key has been
   pressed means the keys are going somewhere else, which is the difference
   between a USB keyboard that works and one that merely enumerated. */
u32  usb_reports(void);

/* One line for the boot log and the system information window. */
const char *usb_describe(void);
