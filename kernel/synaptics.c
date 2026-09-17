/* See include/synaptics.h for what this is and why the detection looks the
 * way it does. This file is the part that runs after a packet arrives.
 *
 * The difference between a trackpad and a mouse is not the hardware, it is
 * the question being answered. A mouse says how far it moved. A trackpad
 * says where the finger is, which is a better answer and a worse one: it
 * has to be turned back into movement, and the turning is where all the
 * mistakes live.
 *
 * Three of them, all of which this has:
 *
 *   - A finger that lifts and lands somewhere else must not drag the
 *     pointer across the screen. The first report of a new contact sets the
 *     origin and moves nothing.
 *   - A pad is about four thousand units across and a screen is a thousand
 *     pixels, so the movement is divided down. Dividing throws away slow
 *     movement unless the remainder is kept, and a pointer that cannot be
 *     moved slowly is a pointer that cannot be aimed.
 *   - A second finger landing changes which contact the pad is reporting,
 *     and the jump that causes looks exactly like a very fast flick. Past a
 *     distance no finger travels in one report, the movement is dropped and
 *     only the origin moves.
 */
#include "synaptics.h"
#include "mouse.h"
#include "ps2.h"
#include "timer.h"

/* --- talking to it -------------------------------------------------------- */

#define CMD_SET_RESOLUTION  0xE8
#define CMD_STATUS_REQUEST  0xE9
#define CMD_SET_SAMPLE_RATE 0xF3

#define QUERY_IDENTIFY      0x00
#define QUERY_CAPABILITIES  0x02
#define SYNAPTICS_MAGIC     0x47   /* the middle byte of every answer */

#define MODE_ABSOLUTE       0x80
#define MODE_HIGH_RATE      0x40
#define MODE_DISABLE_GESTURE 0x04  /* its own gestures, not ours */
#define MODE_W              0x01   /* report how wide the contact is */

/* Bit 23 of the capabilities word: the pad will report W at all. Without it
   there is no way to count fingers and this behaves as a one finger pad. */
#define CAP_EXTENDED        0x800000u

static bool present;
static u8   major, minor;
static u32  caps;
static u32  packets;
static bool w_reported;

static void mouse_cmd(u8 cmd) {
    ps2_command(0xD4);
    ps2_write_data(cmd);
    ps2_read(0);
}

/* An argument, smuggled through four set-resolution commands two bits at a
   time, most significant first. There is no command that takes an argument,
   so this is the argument. */
static void knock(u8 arg) {
    for (int shift = 6; shift >= 0; shift -= 2) {
        mouse_cmd(CMD_SET_RESOLUTION);
        mouse_cmd((u8)((arg >> shift) & 3));
    }
}

static bool query(u8 arg, u8 out[3]) {
    knock(arg);
    ps2_command(0xD4);
    ps2_write_data(CMD_STATUS_REQUEST);
    ps2_read(0);                                  /* the acknowledgement */
    for (int i = 0; i < 3; i++)
        if (!ps2_read(&out[i])) return false;
    return true;
}

bool syn_answer_is_pad(const u8 answer[3]) {
    return answer[1] == SYNAPTICS_MAGIC;
}

bool syn_detect(void) {
    present = false;
    packets = 0;
    syn_reset_state(false);

    if (!ps2_present()) return false;

    u8 id[3];
    if (!query(QUERY_IDENTIFY, id)) return false;

    /* The whole identification. A mouse answering the same four commands
       reports its resolution, and nothing about a resolution is 0x47. */
    if (!syn_answer_is_pad(id)) return false;

    minor = id[0];
    major = (u8)(id[2] & 0x0F);

    u8 cap[3];
    if (query(QUERY_CAPABILITIES, cap) && syn_answer_is_pad(cap))
        caps = ((u32)cap[0] << 16) | ((u32)cap[1] << 8) | cap[2];
    else
        caps = 0;

    /* W is asked for only when the pad said it has it. Asking for a mode a
       pad does not have is how a working pointer becomes a dead one, and
       one finger with no scrolling still beats nothing. */
    w_reported = (caps & CAP_EXTENDED) != 0;

    u8 mode = MODE_ABSOLUTE | MODE_HIGH_RATE | MODE_DISABLE_GESTURE;
    if (w_reported) mode |= MODE_W;

    knock(mode);
    mouse_cmd(CMD_SET_SAMPLE_RATE);
    mouse_cmd(0x14);                              /* commits the mode byte */

    present = true;
    return true;
}

bool syn_present(void)    { return present; }
u8   syn_major(void)      { return major; }
u8   syn_minor(void)      { return minor; }
u32  syn_capabilities(void) { return caps; }
u32  syn_packets(void)    { return packets; }

/* --- turning a position into a pointer ------------------------------------ */

/* Pressure below this is a finger on its way off rather than a finger. */
#define Z_TOUCH        30

/* Pad units per pixel. A pad is a few thousand units across whatever its
   physical size, so this is a feel rather than a measurement. */
#define MOVE_DIVISOR   6

/* Pad units per step of the wheel. */
#define SCROLL_DIVISOR 120

/* Further than a finger travels between two reports.
 *
 * This was 200, which is wrong and was caught by the checks: a pad is about
 * four thousand units across and reports eighty times a second, so a quick
 * flick is a couple of hundred units a report and a limit of 200 threw away
 * ordinary fast movement. What this is for is the jump when the pad starts
 * reporting a different contact, and the honest way to catch that is to
 * notice the finger count changing, which is done below. So this is only
 * the last resort, set where no hand can reach: 700 units in a report is a
 * finger crossing the pad five times a second. */
#define JUMP_LIMIT     700

/* A tap is a contact that ends quickly and did not go anywhere. */
#define TAP_TICKS      20        /* at 100 Hz */
#define TAP_SLOP       120       /* pad units, total */

/* And is held long enough afterwards to be seen. The window manager reads
   the buttons once a pass, so a press and a release between two passes is a
   click that never happened. */
#define TAP_HOLD_TICKS 8

static bool touching;
static i32  last_x, last_y;
static i32  rem_x, rem_y, rem_scroll;

static u8   fingers;             /* on the pad now */
static u8   most_fingers;        /* the most seen during this contact */
static u64  touch_began;
static i32  travelled;

static u8   phys_buttons;
static u8   tap_button;
static u64  tap_release_at;

static u8   buf[6];
static u8   phase;

void syn_reset_state(bool report_w) {
    w_reported = report_w;
    touching = false;
    last_x = last_y = 0;
    rem_x = rem_y = rem_scroll = 0;
    fingers = most_fingers = 0;
    touch_began = 0;
    travelled = 0;
    phys_buttons = 0;
    tap_button = 0;
    tap_release_at = 0;
    phase = 0;
}

u8 syn_fingers(void) { return fingers; }

static i32 abs32(i32 v) { return v < 0 ? -v : v; }

/* How many fingers a width means. Under W mode the width doubles as a count:
   the two smallest values are reserved for saying "more than one", and
   anything from four up is one finger and how wide it is. */
static u8 fingers_from_w(u8 w) {
    if (!w_reported) return 1;
    switch (w) {
        case 0:  return 2;
        case 1:  return 3;
        default: return 1;       /* 2 is a pen, 4 and up is one finger */
    }
}

static void publish(i32 dx, i32 dy) {
    mouse_inject(dx, dy, (u8)(phys_buttons | tap_button));
}

/* Scaled movement, with the remainder kept. Without the remainder a slow
   finger produces a run of zeros and the pointer does not move at all. */
static i32 scaled(i32 delta, i32 divisor, i32 *remainder) {
    i32 total = delta + *remainder;
    i32 out = total / divisor;
    *remainder = total - out * divisor;
    return out;
}

static void contact_ended(void) {
    u64 held = timer_ticks() - touch_began;

    if (held < TAP_TICKS && travelled < TAP_SLOP && most_fingers > 0) {
        /* One finger taps with the left button, two with the right, which
           is the arrangement every pad that has ever worked used. */
        tap_button = most_fingers >= 2 ? 0x02 : 0x01;
        tap_release_at = timer_ticks() + TAP_HOLD_TICKS;
    }

    touching = false;
    fingers = 0;
    most_fingers = 0;
    rem_x = rem_y = rem_scroll = 0;
    publish(0, 0);
}

static void on_packet(void) {
    packets++;

    i32 x = (i32)(((buf[3] & 0x10) << 8) | ((buf[1] & 0x0F) << 8) | buf[4]);
    i32 y = (i32)(((buf[3] & 0x20) << 7) | ((buf[1] & 0xF0) << 4) | buf[5]);
    i32 z = buf[2];
    u8  w = (u8)(((buf[0] & 0x30) >> 2) | ((buf[0] & 0x04) >> 1)
                 | ((buf[3] & 0x04) >> 2));

    phys_buttons = (u8)(buf[0] & 0x03);

    if (z < Z_TOUCH) {
        if (touching) contact_ended();
        else          publish(0, 0);      /* the buttons still move */
        return;
    }

    u8 now_fingers = fingers_from_w(w);

    if (!touching) {
        /* The first report of a contact sets where it started and moves
           nothing. Skipping this is how the pointer jumps across the screen
           when a finger lifts in one corner and lands in another. */
        touching = true;
        fingers = now_fingers;
        most_fingers = now_fingers;
        last_x = x;
        last_y = y;
        touch_began = timer_ticks();
        travelled = 0;
        publish(0, 0);
        return;
    }

    if (now_fingers != fingers) {
        /* A finger arrived or left. The pad reports one position however
           many are on it, and which one it picks changes: the difference
           between the old contact and the new one is not a movement, and
           putting a second finger down to scroll would otherwise throw the
           pointer across the screen first. The clock is left alone, so a
           finger that has been resting does not become a tap by having
           another join it. */
        fingers = now_fingers;
        if (now_fingers > most_fingers) most_fingers = now_fingers;
        last_x = x;
        last_y = y;
        rem_x = rem_y = rem_scroll = 0;
        publish(0, 0);
        return;
    }

    i32 dx = x - last_x;
    i32 dy = y - last_y;
    last_x = x;
    last_y = y;

    if (abs32(dx) > JUMP_LIMIT || abs32(dy) > JUMP_LIMIT) {
        publish(0, 0);                    /* a different contact, not a flick */
        return;
    }

    travelled += abs32(dx) + abs32(dy);

    if (fingers >= 2) {
        /* Two fingers scroll rather than move. Pushing them up the pad
           sends the page up, which is a wheel turning away from you. */
        i32 steps = scaled(-dy, SCROLL_DIVISOR, &rem_scroll);
        if (steps) mouse_inject_scroll(steps);
        publish(0, 0);
        return;
    }

    publish(scaled(dx, MOVE_DIVISOR, &rem_x),
            scaled(dy, MOVE_DIVISOR, &rem_y));
}

void syn_byte(u8 b) {
    /* Two anchors rather than one. The first byte of a report has its top
       two bits 10 and the fourth has 11, so a stream that slips out of step
       is caught within one report instead of decoding six bytes of nonsense
       into a pointer that flies off. */
    if (phase == 0 && (b & 0xC0) != 0x80) return;
    if (phase == 3 && (b & 0xC0) != 0xC0) { phase = 0; return; }

    buf[phase++] = b;
    if (phase == 6) {
        phase = 0;
        on_packet();
    }
}

/* Called from the timer, because the release of a tap is not caused by
   anything the pad sends: the finger is already gone. */
void syn_tick(void) {
    if (!tap_button) return;
    if (timer_ticks() < tap_release_at) return;
    tap_button = 0;
    publish(0, 0);
}
