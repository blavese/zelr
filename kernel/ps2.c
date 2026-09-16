/* The 8042 keyboard controller.
 *
 * Both PS/2 devices come through one controller, one status register and one
 * data port, and the controller will not deliver a second byte until the
 * first has been read. That last part is the whole reason this file exists.
 *
 * The interrupt is raised on the edge of a byte arriving. If a byte is
 * already sitting in the output buffer when the kernel unmasks the line, the
 * edge happened before anything was listening, and no further interrupt is
 * ever raised, because the controller is still holding the byte nobody took.
 * The keyboard and the mouse are then both dead until the machine is power
 * cycled. It takes one keypress during boot to arrange, which is a thing
 * people do constantly, and it looked from the outside like the driver
 * working on some machines and not others.
 *
 * So: every byte is read through one path, and that path is reachable from
 * the timer as well as from the two interrupt handlers. A missed edge costs
 * ten milliseconds now instead of costing the session.
 */
#include "ps2.h"
#include "keyboard.h"
#include "mouse.h"
#include "io.h"
#include "blackbox.h"

#define PS2_DATA 0x60
#define PS2_CMD  0x64
#define PS2_STAT 0x64

/* Status register */
#define ST_OUTPUT   0x01        /* a byte is waiting to be read */
#define ST_INPUT    0x02        /* the controller is still taking the last one */
#define ST_AUX      0x20        /* and it came from the mouse, not the keyboard */

/* Configuration byte */
#define CFG_KBD_IRQ    0x01
#define CFG_AUX_IRQ    0x02
#define CFG_KBD_CLOCK  0x10     /* set means the clock is off */
#define CFG_AUX_CLOCK  0x20     /* same */
#define CFG_TRANSLATE  0x40     /* scancode set 2 arrives looking like set 1 */

/* Commands */
#define CMD_READ_CONFIG   0x20
#define CMD_WRITE_CONFIG  0x60
#define CMD_AUX_OFF       0xA7
#define CMD_AUX_ON        0xA8
#define CMD_KBD_OFF       0xAD
#define CMD_KBD_ON        0xAE

#define KBD_ENABLE_SCAN   0xF4
#define KBD_ECHO          0xEE
#define KBD_ACK           0xFA

/* Long enough for a controller that is there, short enough to give up on one
   that is not. An unanswered port reads 0xFF, so every bit is set, so a wait
   for a bit to clear never ends unless it is bounded. */
#define SPIN 100000

static bool present;
static u8   first_status = 0xFF;
static u8   first_config;
static u32  rescued;

bool ps2_present(void)     { return present; }
u8   ps2_first_status(void){ return first_status; }
u8   ps2_first_config(void){ return first_config; }
u32  ps2_rescued(void)     { return rescued; }

static void wait_write(void) {
    for (u32 i = 0; i < SPIN; i++)
        if (!(inb(PS2_STAT) & ST_INPUT)) return;
}

static bool wait_read(void) {
    for (u32 i = 0; i < SPIN; i++)
        if (inb(PS2_STAT) & ST_OUTPUT) return true;
    return false;
}

void ps2_command(u8 cmd)    { wait_write(); outb(PS2_CMD, cmd); }
void ps2_write_data(u8 b)   { wait_write(); outb(PS2_DATA, b); }

bool ps2_read(u8 *out) {
    if (!wait_read()) return false;
    u8 b = inb(PS2_DATA);
    if (out) *out = b;
    return true;
}

u8 ps2_config(void) {
    u8 c = 0;
    ps2_command(CMD_READ_CONFIG);
    ps2_read(&c);
    return c;
}

static void write_config(u8 c) {
    ps2_command(CMD_WRITE_CONFIG);
    ps2_write_data(c);
}

/* Everything the firmware left behind, thrown away. Bounded, because a
   controller that is stuck reporting a byte it will not hand over would
   otherwise be an infinite loop at boot. */
static void drain(void) {
    for (u32 i = 0; i < 64; i++) {
        u8 st = inb(PS2_STAT);
        if (st == 0xFF || !(st & ST_OUTPUT)) return;
        (void)inb(PS2_DATA);
    }
}

u32 ps2_poll(void) {
    if (!present) return 0;

    u32 took = 0;
    /* Bounded for the same reason drain is, and because this runs inside an
       interrupt handler: a controller gone mad must not be able to hold the
       machine here. */
    for (u32 i = 0; i < 64; i++) {
        u8 st = inb(PS2_STAT);
        if (st == 0xFF || !(st & ST_OUTPUT)) break;

        u8 b = inb(PS2_DATA);
        took++;

        /* Which device it came from is in the status byte read before the
           data, not after: reading the data is what clears it. */
        if (st & ST_AUX) mouse_byte(b);
        else             keyboard_byte(b);
    }
    return took;
}

void ps2_poll_from_timer(void) {
    /* Anything found here is a byte no interrupt arrived for. Counted, so
       that a machine whose keyboard interrupt is not being delivered at all
       says so in the boot log rather than merely feeling slow. */
    rescued += ps2_poll();
}

void ps2_init(void) {
    present = false;
    rescued = 0;

    first_status = inb(PS2_STAT);
    if (first_status == 0xFF) {
        bb_log("ps/2 no controller answers");
        return;                       /* no 8042, which is most laptops */
    }

    /* Both ports off first. Nothing should arrive in the middle of setting
       the controller up, and a device still streaming from before the reboot
       is exactly what would. */
    /* Read before anything is switched off, because switching a port off is
       itself a change to this byte, and what is wanted here is what the
       firmware left rather than what this function just did. */
    first_config = ps2_config();

    ps2_command(CMD_KBD_OFF);
    ps2_command(CMD_AUX_OFF);
    drain();

    /* Written rather than adjusted. What the firmware leaves here differs
       between a cold boot and a warm one, and between two machines with the
       same firmware configured differently, so reading it and setting one
       bit means inheriting whatever else it happened to contain. Translation
       especially: kernel/keyboard.c decodes scancode set 1, and with this bit
       clear the keyboard sends set 2 and every key decodes as the wrong one. */
    u8 c = first_config;
    c |= CFG_KBD_IRQ | CFG_AUX_IRQ | CFG_TRANSLATE;
    c &= (u8)~(CFG_KBD_CLOCK | CFG_AUX_CLOCK);
    write_config(c);

    ps2_command(CMD_KBD_ON);
    ps2_command(CMD_AUX_ON);

    /* And the keyboard itself has to be told to send. Firmware often leaves
       it disabled on the way out, and a controller listening to a keyboard
       that is not talking looks identical to a driver that does not work. */
    ps2_write_data(KBD_ENABLE_SCAN);
    drain();

    /* And then ask the keyboard to say something back.
     *
     * Configured is not the same as present and talking. The echo command is
     * answered by the keyboard itself rather than by the controller, so a
     * reply means the clock is running, the port is open and the device is
     * on the other end of it. Nothing is done differently either way: it is
     * recorded because on a machine where typing does not work this line is
     * the difference between a controller problem and everything else. */
    ps2_write_data(KBD_ECHO);
    u8 reply = 0;
    bool answered = ps2_read(&reply) && (reply == KBD_ECHO || reply == KBD_ACK);
    drain();

    present = true;
    bb_log("ps/2 status %x, config %x to %x, keyboard %s",
           first_status, first_config, ps2_config(),
           answered ? "answers" : "silent");
}
