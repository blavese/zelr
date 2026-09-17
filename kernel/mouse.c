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
#include "synaptics.h"
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

/* --- button changes, kept until somebody reads them ----------------------
 *
 * The window manager reads the mouse once a pass of its loop, and a pass
 * that ends in compositing the whole screen is not short. A press and a
 * release that both happen inside one of them are invisible to anything
 * reading the level: the state before is up, the state after is up, and
 * nothing in between ever happened.
 *
 * That is not a theoretical loss. It is a click on a busy desktop doing
 * nothing at all, with no sign of why, and it is what several checks here
 * kept failing on: a swatch clicked three times running that never changed
 * the colour, a menu entry that would not launch.
 *
 * So each change is kept, with where the pointer was when it happened,
 * until it is taken. The position matters as much as the change: a click is
 * where the button went down, not where the pointer ended up. */
#define MOUSE_EDGES 32
static mouse_edge_t edges[MOUSE_EDGES];
static volatile u32 edge_head, edge_tail;

static void edge_record(void) {
    u32 next = (edge_head + 1) % MOUSE_EDGES;
    if (next == edge_tail) return;          /* nobody is reading; keep the backlog */
    edges[edge_head].x = mx;
    edges[edge_head].y = my;
    edges[edge_head].buttons = buttons;
    edge_head = next;
}

bool mouse_take_edge(mouse_edge_t *out) {
    if (edge_head == edge_tail) return false;
    *out = edges[edge_tail];
    edge_tail = (edge_tail + 1) % MOUSE_EDGES;
    return true;
}
static u8   packet[4];
static u8   phase;
static u8   packet_len = 3;      /* four once the wheel is switched on */
static i32  wheel;
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
/* A trackpad has no wheel and scrolls anyway, with two fingers, which is
   what anything asking this actually wants to know. */
bool mouse_has_wheel(void) { return packet_len == 4 || syn_present(); }

i32 mouse_take_scroll(void) {
    /* Read and cleared together, with interrupts off: a packet arriving
       between the two would be a turn of the wheel nobody ever saw. */
    bool were_on = interrupts_enabled();
    cli();
    i32 n = wheel;
    wheel = 0;
    if (were_on) sti();
    return n;
}

void mouse_inject_scroll(i32 steps) {
    wheel += steps;
    moves++;
}

static void mouse_cmd(u8 cmd) {
    ps2_command(0xD4);                      /* the next byte is for the mouse */
    ps2_write_data(cmd);
    ps2_read(0);                            /* and it acknowledges each one */
}

/* The knock that turns a two button mouse into one that reports a wheel.
 *
 * Three sample rates in a fixed order, which no ordinary sequence of
 * commands would produce by accident, and then asking the device who it is.
 * One with a wheel answers 3 and sends four byte packets from then on; one
 * without answers 0 and nothing about it changes. The rate is put back
 * afterwards because the knock leaves it at 80 reports a second, which is a
 * visibly coarser pointer. */
static bool enable_wheel(void) {
    static const u8 knock[3] = { 200, 100, 80 };
    for (u32 i = 0; i < 3; i++) {
        mouse_cmd(0xF3);
        mouse_cmd(knock[i]);
    }

    ps2_command(0xD4);
    ps2_write_data(0xF2);                   /* get device id */
    ps2_read(0);                            /* the acknowledgement */

    u8 id = 0;
    if (!ps2_read(&id)) return false;

    mouse_cmd(0xF3);
    mouse_cmd(100);
    return id == 3;
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
    u8 was = buttons;
    buttons = btns;

    if (dx || dy) {
        mouse_hide();
        mx += dx;
        my -= dy;                               /* screen y grows downward */
        i32 maxx = (i32)fb_width() - 1;
        i32 maxy = (i32)fb_height() - 1;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > maxx) mx = maxx;
        if (my > maxy) my = maxy;
        moves++;
        mouse_show();
    }

    /* After the move, not before it: a packet can carry both, and where the
       button went down is where the pointer ended up. */
    if (was != btns) edge_record();
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

    if (packet_len == 4) {
        /* The low four bits are a signed count of steps. The rest carries
           the fourth and fifth buttons on mice that have them, which
           nothing here uses. */
        i32 z = packet[3] & 0x0F;
        if (z & 0x08) z -= 16;
        if (z) mouse_inject_scroll(z);
    }
}

/* One byte of a packet, already taken off the controller.
 *
 * Which device a byte came from can only be told from the status register as
 * it was before the read, so kernel/ps2.c does the reading and the sorting
 * and this is handed the ones that were the mouse's. */
void mouse_byte(u8 b) {
    /* A trackpad in absolute mode sends six byte reports that mean something
       else entirely, so it decodes its own. Everything after that arrives
       back here through mouse_inject, which is the same door the USB mouse
       comes in by. */
    if (syn_present()) { syn_byte(b); return; }

    packet[phase++] = b;
    if (phase == 1 && !(packet[0] & 0x08)) { phase = 0; return; }
    if (phase == packet_len) { phase = 0; on_packet(); }
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

    /* A trackpad first, because one answering as a plain mouse is what the
       last thirty years of laptops have been putting up with. It either
       identifies itself or it does not, and the knock leaves a mouse exactly
       where it found it, so there is nothing to undo when it does not. */
    if (syn_detect()) {
        packet_len = 6;
    } else {
        packet_len = enable_wheel() ? 4 : 3;
    }
    wheel = 0;
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
