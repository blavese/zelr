/* PS/2 keyboard, scancode set 1, with a small ring buffer so keystrokes
   taken during an interrupt survive until someone reads them. */
#include "keyboard.h"
#include "idt.h"
#include "pic.h"
#include "io.h"
#include "ps2.h"
#include "serial.h"

/* The buffer holds ints rather than chars, because a key is not always a
   character: an arrow or a page key has no letter to stand for it. */
#define BUFSZ 256
static volatile int buf[BUFSZ];
static volatile u32 head = 0, tail = 0;

static bool shift = false, caps = false, ctrl = false, alt = false;
static bool extended = false;    /* the last byte was the 0xE0 prefix */

static const char MAP[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  0x5C,'z','x','c','v','b','n','m',',','.','/',
    0,  '*', 0, ' ',
};

static const char MAP_SHIFT[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',
    0,  '*', 0, ' ',
};

static void push(int c) {
    u32 next = (head + 1) % BUFSZ;
    /* Stamped with what was held down now, because that is the only moment
       it is known. Whoever reads this may do so long afterwards. */
    if (alt)   c |= KEY_MOD_ALT;
    if (ctrl)  c |= KEY_MOD_CTRL;
    if (shift) c |= KEY_MOD_SHIFT;

    if (next != tail) { buf[head] = c; head = next; }
}

/* The scancodes that arrive behind a 0xE0 prefix. Everything here is a key
   rather than a character, which is why they get their own numbers. */
static int extended_key(u8 code) {
    switch (code) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PAGE_UP;
        case 0x51: return KEY_PAGE_DOWN;
        case 0x53: return KEY_DELETE;
        case 0x52: return KEY_INSERT;
        default:   return -1;
    }
}

/* The same keys again, for a keyboard with the numeric keypad's number lock
   off, which sends them without the prefix. */
static int keypad_key(u8 code) {
    switch (code) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PAGE_UP;
        case 0x51: return KEY_PAGE_DOWN;
        case 0x53: return KEY_DELETE;
        default:   return -1;
    }
}

/* One scancode, already taken off the controller by kernel/ps2.c.
 *
 * It reads the port rather than this, because the byte might have been the
 * mouse's, and that can only be told from the status register as it was
 * before the read. */
void keyboard_byte(u8 sc) {

    /* A prefix on its own. The byte after it is what was actually pressed. */
    if (sc == 0xE0) { extended = true; return; }

    if (sc & 0x80) {                       /* key release */
        u8 code = sc & 0x7F;
        if (!extended) {
            if (code == 0x2A || code == 0x36) shift = false;
            if (code == 0x1D) ctrl = false;
            if (code == 0x38) alt = false;
        } else {
            if (code == 0x1D) ctrl = false;   /* the right hand ones */
            if (code == 0x38) alt = false;
        }
        extended = false;
        return;
    }

    if (extended) {
        extended = false;
        if (sc == 0x1D) { ctrl = true; return; }
        if (sc == 0x38) { alt = true; return; }
        int k = extended_key(sc);
        if (k >= 0) push(k);
        return;
    }

    switch (sc) {
        case 0x2A: case 0x36: shift = true; return;
        case 0x1D: ctrl = true; return;
        case 0x38: alt = true; return;
        case 0x3A: caps = !caps; return;
        default: break;
    }

    /* The function keys, which have no character either. */
    if (sc >= 0x3B && sc <= 0x44) { push(KEY_F1 + (sc - 0x3B)); return; }
    if (sc == 0x57) { push(KEY_F1 + 10); return; }
    if (sc == 0x58) { push(KEY_F1 + 11); return; }

    if (sc >= 128) return;
    char c = shift ? MAP_SHIFT[sc] : MAP[sc];

    /* A keypad with number lock off sends the navigation keys unprefixed. */
    if (!c) {
        int k = keypad_key(sc);
        if (k >= 0) push(k);
        return;
    }

    if (caps && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    else if (caps && shift && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    if (ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);   /* ^A..^Z */
    push(c);
}

static void on_irq(registers_t *r) {
    (void)r;
    ps2_poll();
}

void keyboard_init(void) {
    register_interrupt_handler(33, on_irq);
    pic_unmask(1);
}

bool kbd_has_char(void) { return head != tail; }

/* See include/keyboard.h. The key is already stamped with its modifiers,
   because the keyboard that sent it said what they were. */
void keyboard_inject(int key) {
    u32 next = (head + 1) % BUFSZ;
    if (next != tail) { buf[head] = key; head = next; }
}

void keyboard_set_mods(bool a, bool c, bool s) {
    alt = a; ctrl = c; shift = s;
}

bool kbd_alt(void)   { return alt; }
bool kbd_ctrl(void)  { return ctrl; }
bool kbd_shift(void) { return shift; }

int kbd_trygetchar(void) {
    if (head == tail) {
        /* Also accept input from the serial line, which is how the
           automated tests drive the shell. It arrives on IRQ4 and is
           buffered there, so nothing is lost between polls. */
        int s = serial_trygetc();
        if (s >= 0) return s == 13 ? 10 : s;   /* CR becomes LF */
        return -1;
    }
    int c = buf[tail];
    tail = (tail + 1) % BUFSZ;

    /* A special key is already an int above 255; a character has to come
       back unsigned or anything above 127 arrives negative and reads as
       "nothing here". Either way the modifier bits ride along on top. */
    int mods = c & KEY_MODS;
    int code = KEY_CODE(c);
    return mods | (KEY_IS_SPECIAL(code) ? code : (int)(u8)code);
}

char kbd_getchar(void) {
    for (;;) {
        int c = kbd_trygetchar();
        if (c >= 0) return (char)KEY_CODE(c);
        __asm__ volatile ("hlt");
    }
}
