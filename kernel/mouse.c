/* PS/2 mouse on IRQ12.
 *
 * The controller multiplexes keyboard and mouse over the same pair of ports,
 * so mouse commands have to be prefixed with 0xD4 to say "this one is for the
 * other device". Data arrives as three byte packets; the first byte has a bit
 * that is always set, which is used here to resynchronise if the stream ever
 * slips out of phase. */
#include "mouse.h"
#include "fb.h"
#include "idt.h"
#include "pic.h"
#include "ps2.h"
#include "io.h"
#include "printf.h"
#include "string.h"

#define PS2_DATA 0x60
#define PS2_CMD  0x64
#define PS2_STAT 0x64

#define CUR_W 12
#define CUR_H 19

/* 0 transparent, 1 outline, 2 fill */
static const u8 CURSOR[CUR_H][CUR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,1,1,1,1,1},
    {1,2,2,2,1,2,2,1,0,0,0,0},
    {1,2,2,1,1,2,2,1,0,0,0,0},
    {1,2,1,0,0,1,2,2,1,0,0,0},
    {1,1,0,0,0,1,2,2,1,0,0,0},
    {1,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

static bool present = false;
static i32  mx, my;
static u8   buttons;
static u8   packet[3];
static u8   phase;
static u32  moves;

static bool drawn;
static bool autodraw = true;   /* off while the window manager owns the pointer */
static u32  saved[CUR_H][CUR_W];
static i32  saved_x, saved_y;

bool mouse_present(void) { return present; }
i32  mouse_x(void) { return mx; }
i32  mouse_y(void) { return my; }
u8   mouse_buttons(void) { return buttons; }
u32  mouse_moves(void) { return moves; }

static void mouse_cmd(u8 cmd) {
    ps2_command(0xD4);                      /* the next byte is for the mouse */
    ps2_write_data(cmd);
    ps2_read(0);                            /* and it acknowledges each one */
}

void mouse_hide(void) {
    if (!drawn || !fb_active()) return;
    for (u32 y = 0; y < CUR_H; y++)
        for (u32 x = 0; x < CUR_W; x++)
            fb_put((u32)(saved_x + (i32)x), (u32)(saved_y + (i32)y), saved[y][x]);
    fb_flush_rect((u32)saved_x, (u32)saved_y, CUR_W, CUR_H);
    drawn = false;
}

void mouse_set_autodraw(bool on) {
    if (!on) mouse_hide();
    autodraw = on;
}

void mouse_show(void) {
    if (!autodraw || drawn || !fb_active() || !present) return;
    saved_x = mx; saved_y = my;
    for (u32 y = 0; y < CUR_H; y++) {
        for (u32 x = 0; x < CUR_W; x++) {
            u32 px = (u32)(mx + (i32)x), py = (u32)(my + (i32)y);
            saved[y][x] = fb_get(px, py);
            u8 v = CURSOR[y][x];
            if (v == 1) fb_put(px, py, RGB(0x10, 0x14, 0x18));
            else if (v == 2) fb_put(px, py, RGB(0xF2, 0xF5, 0xF7));
        }
    }
    fb_flush_rect((u32)mx, (u32)my, CUR_W, CUR_H);
    drawn = true;
}

/* Where the pointer ends up, given movement and buttons. Shared, because a
   USB mouse produces exactly the same three things through an entirely
   different path and there is no reason for the answer to differ. */
void mouse_inject(i32 dx, i32 dy, u8 btns) {
    buttons = btns;
    if (!dx && !dy) return;

    mouse_hide();
    mx += dx;
    my -= dy;                                   /* screen y grows downward */
    i32 maxx = (i32)fb_width() - 1;
    i32 maxy = (i32)fb_height() - 1;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > maxx) mx = maxx;
    if (my > maxy) my = maxy;
    moves++;
    mouse_show();
}

static void on_packet(void) {
    u8 flags = packet[0];
    if (!(flags & 0x08)) { phase = 0; return; }     /* lost sync */
    if (flags & 0xC0) return;                       /* overflow, drop it */

    i32 dx = (i32)packet[1];
    i32 dy = (i32)packet[2];
    if (flags & 0x10) dx |= (i32)0xFFFFFF00;        /* sign extend */
    if (flags & 0x20) dy |= (i32)0xFFFFFF00;

    mouse_inject(dx, dy, flags & 0x07);
}

/* One byte of a packet, already taken off the controller.
 *
 * Which device a byte came from can only be told from the status register as
 * it was before the read, so kernel/ps2.c does the reading and the sorting
 * and this is handed the ones that were the mouse's. */
void mouse_byte(u8 b) {
    packet[phase++] = b;
    if (phase == 1 && !(packet[0] & 0x08)) { phase = 0; return; }
    if (phase == 3) { phase = 0; on_packet(); }
}

static void mouse_isr(registers_t *r) {
    (void)r;
    ps2_poll();
}

bool mouse_init(void) {
    present = false;
    phase = 0;
    buttons = 0;
    moves = 0;
    drawn = false;

    /* The port and the interrupt are the controller's business and were set
       up before this ran. What is left is the device itself. */
    if (!ps2_present()) return false;

    mouse_cmd(0xF6);                                 /* restore defaults */
    mouse_cmd(0xF4);                                 /* start reporting */

    mx = (i32)(fb_active() ? fb_width() / 2 : 0);
    my = (i32)(fb_active() ? fb_height() / 2 : 0);

    register_interrupt_handler(32 + 12, mouse_isr);
    pic_unmask(2);                                   /* cascade to the slave */
    pic_unmask(12);

    present = true;
    mouse_show();
    return true;
}
