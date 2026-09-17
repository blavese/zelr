#pragma once
#include "types.h"

/* A Synaptics trackpad, which is what the 8042 has on the other end of it in
 * most laptops that still have an 8042 at all.
 *
 * Left alone, one of these pretends to be a two button mouse: it sends the
 * same three byte packets a 1987 mouse sent, and everything works, badly.
 * There is no scrolling, no tap, no idea how many fingers are on it, and the
 * firmware's own idea of a gesture is whatever the vendor shipped. Asked
 * properly, it reports where the finger is rather than how far it moved, how
 * hard it is pressing, and how wide the contact is, which is how it says
 * whether there are two fingers on it.
 *
 * The asking is a knock, in the same spirit as the one that turns a mouse
 * into a wheel mouse. There is no command for "are you a trackpad", so the
 * argument is smuggled through four set-resolution commands, two bits at a
 * time, and then a status request reads three bytes back. A real mouse
 * answers with its resolution and nothing matches; a Synaptics answers with
 * 0x47 in the middle byte, which is the whole identification.
 *
 * None of this can be tested in an emulator, because no emulator has one.
 * What is tested is everything after the packet arrives: see the checks in
 * kernel/selftest.c, which feed recorded packets through the decoder and
 * require the pointer, the scrolling and the taps that should come out. */

/* The knock, the identify, and absolute mode. False if there is nothing
   there, which is the ordinary answer on a desktop and in QEMU, and leaves
   the plain mouse driver in charge.
 *
   Has to be called before interrupts are enabled, which is where the mouse
   driver calls it. After that the timer drains the controller on every tick
   and hands whatever it finds to the packet decoder, so the three bytes an
   answer consists of are taken by the timer before the read here sees them:
   the pad is there, answers correctly, and is reported as absent. */
bool syn_detect(void);

/* Whether three bytes from a status request are a pad saying hello. Split
   out from the asking so it can be checked without one to ask, which is the
   only part of the identification an emulator can exercise. */
bool syn_answer_is_pad(const u8 answer[3]);

/* One byte of a packet, from kernel/ps2.c. Six of them make a report. */
void syn_byte(u8 b);

/* From the timer. A tap has to be held for a few ticks to be seen at all,
   and nothing the pad sends ends it: the finger is already gone. */
void syn_tick(void);

bool syn_present(void);

/* For the boot log, because a trackpad that is found and does nothing and a
   trackpad that was never found look identical from the outside. */
u8   syn_major(void);
u8   syn_minor(void);
u32  syn_capabilities(void);
u32  syn_packets(void);
u8   syn_fingers(void);

/* Resets the decoder without touching the hardware, and says whether to
   decode the width field. The checks use this to be a pad of either kind
   without one being present: a decoder that can only be exercised by
   hardware no emulator has is a decoder nothing ever checks. It also keeps
   one check from starting mid gesture because the last one left a finger
   down. */
void syn_reset_state(bool report_w);
