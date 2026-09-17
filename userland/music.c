/* A music player.
 *
 * There is a sound driver and, until now, nothing on the desktop that used
 * it. This plays WAV files, which is the format that is a header and then the
 * samples: no decoding, no tables, nothing to get wrong except the header,
 * and something a person can actually produce on another machine and copy in
 * on a USB stick.
 *
 * The hardware plays at one rate and in stereo and will not be argued with,
 * so a file recorded at any other rate is stepped through at a ratio held in
 * sixteen fixed point bits, and a mono file has each sample written twice.
 * Nearest sample rather than anything cleverer: the arithmetic is integer,
 * this is a music player on an operating system with no floating point, and
 * the difference is audible only to somebody listening for it.
 */
#include "zelr.h"
#include "ui.h"

#define MAX_FILES  64
#define NAME_MAX   64
#define PATH_MAX   128
#define CHUNK      2048          /* frames handed to the driver at a time */

typedef struct {
    char name[NAME_MAX];
    char path[PATH_MAX];
    u32  size;
} song_t;

static song_t songs[MAX_FILES];
static int    nsongs;
static int    selected = -1;
static int    scroll;

/* What is playing, if anything. */
static int   fd = -1;
static int   playing;
static u32   data_at, data_len, data_done;
static u32   src_rate, src_chans, src_bits;
static u32   dev_rate = 48000, dev_chans = 2;
static u32   step;               /* source frames per device frame, 16.16 */
static u32   frac;               /* where we are between two source frames */
static char  status[96];

static short out[CHUNK * 2];
static unsigned char in_buf[CHUNK * 4];

/* --- little helpers -------------------------------------------------------- */

static void copy_str(char *dst, int cap, const char *src) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void song_copy(song_t *dst, const song_t *src) {
    copy_str(dst->name, NAME_MAX, src->name);
    copy_str(dst->path, PATH_MAX, src->path);
    dst->size = src->size;
}

static void join(char *dst, int cap, const char *dir, const char *name) {
    int n = 0;
    for (int i = 0; dir[i] && n < cap - 2; i++) dst[n++] = dir[i];
    if (n && dst[n - 1] != '/') dst[n++] = '/';
    for (int i = 0; name[i] && n < cap - 1; i++) dst[n++] = name[i];
    dst[n] = 0;
}

static int is_wav(const char *name) {
    int n = 0;
    while (name[n]) n++;
    if (n < 5) return 0;
    const char *e = name + n - 4;
    return (e[0] == '.'
            && (e[1] == 'w' || e[1] == 'W')
            && (e[2] == 'a' || e[2] == 'A')
            && (e[3] == 'v' || e[3] == 'V'));
}

static u32 le32(const unsigned char *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 le16(const unsigned char *p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static void say(const char *s) { copy_str(status, sizeof(status), s); }

/* --- finding something to play -------------------------------------------- */

static void scan_dir(const char *dir) {
    zelr_stat st;
    for (int i = 0; nsongs < MAX_FILES && readdir(dir, i, &st) == 1; i++) {
        if (st.is_dir || !is_wav(st.name)) continue;
        song_t *s = &songs[nsongs++];
        copy_str(s->name, NAME_MAX, st.name);
        join(s->path, PATH_MAX, dir, st.name);
        s->size = st.size;
    }
}

static void rescan(void) {
    nsongs = 0;
    selected = -1;
    scan_dir("/home");
    scan_dir("/music");
    scan_dir("/usb");
    if (nsongs) selected = 0;
}

/* --- the header ------------------------------------------------------------
 *
 * RIFF is chunks: four characters of name, four bytes of length, that many
 * bytes. Two of them matter, and they are not guaranteed to be in any
 * particular order or to be the only ones there, so this walks the file
 * rather than assuming the layout every writer happens to use.
 */
static int open_song(const song_t *s) {
    if (fd >= 0) { close(fd); fd = -1; }
    playing = 0;

    fd = open(s->path, O_READ);
    if (fd < 0) { say("cannot open it"); return 0; }

    unsigned char head[12];
    if (fread(fd, head, 12) != 12
        || head[0] != 'R' || head[1] != 'I' || head[2] != 'F' || head[3] != 'F'
        || head[8] != 'W' || head[9] != 'A' || head[10] != 'V' || head[11] != 'E') {
        say("not a wav file");
        close(fd); fd = -1;
        return 0;
    }

    u32 at = 12;
    int have_fmt = 0;
    data_at = data_len = 0;

    for (;;) {
        unsigned char ch[8];
        if (seek(fd, (int)at, SEEK_SET) < 0) break;
        if (fread(fd, ch, 8) != 8) break;
        u32 len = le32(ch + 4);

        if (ch[0] == 'f' && ch[1] == 'm' && ch[2] == 't' && ch[3] == ' ') {
            unsigned char f[16];
            if (fread(fd, f, 16) != 16) break;
            if (le16(f) != 1) { say("only plain pcm"); close(fd); fd = -1; return 0; }
            src_chans = le16(f + 2);
            src_rate  = le32(f + 4);
            src_bits  = le16(f + 14);
            have_fmt = 1;
        } else if (ch[0] == 'd' && ch[1] == 'a' && ch[2] == 't' && ch[3] == 'a') {
            data_at = at + 8;
            data_len = len;
            break;
        }

        at += 8 + len + (len & 1);          /* chunks are padded to even */
    }

    if (!have_fmt || !data_at || !src_rate || !src_chans
        || (src_bits != 16 && src_bits != 8)) {
        say("that header is not one this can read");
        close(fd); fd = -1;
        return 0;
    }

    data_done = 0;
    frac = 0;
    step = (u32)(((u64)src_rate << 16) / (dev_rate ? dev_rate : 48000));
    seek(fd, (int)data_at, SEEK_SET);

    char msg[96];
    int n = 0;
    n += utoa(src_rate, msg + n);
    const char *hz = " Hz, ";
    for (int i = 0; hz[i]; i++) msg[n++] = hz[i];
    n += utoa(src_bits, msg + n);
    const char *bit = " bit, ";
    for (int i = 0; bit[i]; i++) msg[n++] = bit[i];
    const char *ch2 = src_chans == 1 ? "mono" : "stereo";
    for (int i = 0; ch2[i]; i++) msg[n++] = ch2[i];
    msg[n] = 0;
    say(msg);

    playing = 1;
    return 1;
}

/* One block: read source frames, step through them at the ratio, write the
   device's own rate out. Returns 0 when the file is finished. */
static int pump(void) {
    if (fd < 0 || !playing) return 0;

    u32 src_frame = src_chans * (src_bits / 8);
    u32 left = data_len - data_done;
    if (!left) return 0;

    /* How many source frames this block will consume, which is how many to
       read: the step is how far along the source one output frame moves. */
    u32 want_src = ((u32)CHUNK * step + frac) >> 16;
    if (want_src < 1) want_src = 1;
    if (want_src > CHUNK) want_src = CHUNK;

    u32 bytes = want_src * src_frame;
    if (bytes > sizeof(in_buf)) bytes = (sizeof(in_buf) / src_frame) * src_frame;
    if (bytes > left) bytes = (left / src_frame) * src_frame;
    if (!bytes) return 0;

    int got = fread(fd, in_buf, (int)bytes);
    if (got <= 0) return 0;
    data_done += (u32)got;

    u32 have = (u32)got / src_frame;
    u32 made = 0;
    u32 pos = frac;

    while (made < CHUNK) {
        u32 i = pos >> 16;
        if (i >= have) break;

        short l, r;
        if (src_bits == 16) {
            const unsigned char *p = in_buf + i * src_frame;
            l = (short)(le16(p) & 0xFFFF);
            r = src_chans > 1 ? (short)(le16(p + 2) & 0xFFFF) : l;
        } else {
            const unsigned char *p = in_buf + i * src_frame;
            l = (short)(((int)p[0] - 128) << 8);
            r = src_chans > 1 ? (short)(((int)p[1] - 128) << 8) : l;
        }

        out[made * 2] = l;
        out[made * 2 + 1] = r;
        made++;
        pos += step;
    }

    frac = pos - ((pos >> 16) << 16);      /* what is left over for next time */
    if (made) sound_write(out, (int)made);
    return made > 0;
}

/* --- something to play on a machine with no files ------------------------- */

static const struct { u32 hz; u32 ms; } TUNE[] = {
    { 523, 180 }, { 587, 180 }, { 659, 180 }, { 523, 180 },
    { 659, 220 }, { 784, 260 }, { 659, 180 }, { 523, 320 },
};
#define TUNE_N ((int)(sizeof(TUNE) / sizeof(TUNE[0])))

static int tune_at = -1;
static int tune_until;

/* A note is written as a block of sine and then left to drain, so the window
   is still being drawn while it plays rather than after. */
static void tune_step(void) {
    if (tune_at < 0) return;
    if (ticks() < tune_until) return;

    if (tune_at >= TUNE_N) { tune_at = -1; say("done"); return; }

    u32 hz = TUNE[tune_at].hz, ms = TUNE[tune_at].ms;
    u32 total = (dev_rate * ms) / 1000;
    u32 phase = 0;
    u32 stepf = (u32)(((u64)hz << 16) / dev_rate);

    while (total) {
        u32 n = total > CHUNK ? CHUNK : total;
        for (u32 i = 0; i < n; i++) {
            /* A parabola for half a turn and its mirror for the other, which
               is a sine to within a few per cent and needs no table. */
            u32 half = phase & 0x7FFF;
            int y = (int)((half * (32768u - half)) >> 13);
            if (phase & 0x8000) y = -y;
            short v = (short)(y * 3);
            out[i * 2] = v;
            out[i * 2 + 1] = v;
            phase = (phase + stepf) & 0xFFFF;
        }
        sound_write(out, (int)n);
        total -= n;
    }

    tune_at++;
    tune_until = ticks();
}

void _start(void) {
    int win = win_create("Music", 520, 420);
    if (win < 0) exit(1);
    win_allow_resize(win);

    zelr_sound snd;
    if (sound_info(&snd) == 0 && snd.present) {
        dev_rate = snd.rate ? snd.rate : 48000;
        dev_chans = snd.channels ? snd.channels : 2;
        say("ready");
    } else {
        say("this machine has no sound hardware");
    }

    rescan();

    /* Started on a file: play that one rather than waiting to be asked. */
    char wanted[PATH_MAX];
    if (getarg(wanted, sizeof(wanted)) > 0 && wanted[0]) {
        song_t one;
        copy_str(one.path, PATH_MAX, wanted);
        const char *leaf = wanted;
        for (const char *p = wanted; *p; p++) if (*p == '/') leaf = p + 1;
        copy_str(one.name, NAME_MAX, leaf);
        one.size = 0;

        /* Put it at the top of the list as well, so it is the thing that is
           selected and not just the thing that is playing. */
        if (nsongs < MAX_FILES) {
            /* Copied a field at a time. A struct assignment of this size
               becomes a call to memcpy, and there is no library here to
               have one in. */
            for (int i = nsongs; i > 0; i--) song_copy(&songs[i], &songs[i - 1]);
            song_copy(&songs[0], &one);
            nsongs++;
            selected = 0;
        }
        open_song(&one);
    }

    ui_input in;
    memset(&in, 0, sizeof(in));

    for (;;) {
        int w = win_width(win), h = win_height(win);
        u32 *px = win_surface(win);
        if (!px || w <= 0 || h <= 0) break;
        surface s = { px, w, h };
        ui_theme t = ui_load_theme();

        ui_begin(&in);
        win_event ev;
        int closing = 0;
        while (win_poll(win, &ev)) {
            if (ev.type == WIN_EV_CLOSE) { closing = 1; break; }
            ui_feed(&in, &ev);
        }
        if (closing) break;

        if (playing && !pump()) {
            playing = 0;
            close(fd);
            fd = -1;
            say("finished");
        }
        tune_step();

        fill(&s, t.bg);
        ui_toolbar(&s, &t, w, UI_ROW + UI_PAD);

        int bx = UI_PAD;
        if (ui_button(&s, &in, &t, bx, UI_PAD / 2, 80,
                      playing ? "Stop" : "Play")) {
            if (playing) {
                playing = 0;
                if (fd >= 0) { close(fd); fd = -1; }
                say("stopped");
            } else if (selected >= 0) {
                open_song(&songs[selected]);
            }
        }
        bx += 86;
        if (ui_button(&s, &in, &t, bx, UI_PAD / 2, 80, "Tune")) {
            tune_at = 0;
            tune_until = ticks();
            say("playing a few notes");
        }
        bx += 86;
        if (ui_button(&s, &in, &t, bx, UI_PAD / 2, 90, "Look again"))
            rescan();

        int list_y = UI_ROW + UI_PAD * 2;
        int foot = UI_ROW + 28;
        int list_h = h - list_y - foot;
        int shown = list_h / UI_ROW;
        if (shown < 1) shown = 1;

        if (in.scroll) scroll += in.scroll;
        if (scroll > nsongs - shown) scroll = nsongs - shown;
        if (scroll < 0) scroll = 0;

        if (!nsongs) {
            face_draw(&s, UI_PAD * 2, list_y + UI_PAD,
                      "No .wav files in /home, /music or /usb.",
                      t.dim, UI_FACE_BODY);
            face_draw(&s, UI_PAD * 2, list_y + UI_PAD + 22,
                      "Copy one onto a stick and plug it in, or press Tune.",
                      t.dim, UI_FACE_BODY);
        }

        for (int i = 0; i < shown && scroll + i < nsongs; i++) {
            int idx = scroll + i;
            int ry = list_y + i * UI_ROW;
            int over = ui_hit(&in, UI_PAD, ry, w - UI_PAD * 2, UI_ROW);

            if (idx == selected)
                round_rect(&s, UI_PAD, ry, w - UI_PAD * 2, UI_ROW, 4, t.soft);
            else if (over)
                round_rect(&s, UI_PAD, ry, w - UI_PAD * 2, UI_ROW, 4,
                           mix(t.bg, t.fg, 14));

            if (over && in.released) {
                in.released = 0;
                if (selected == idx) open_song(&songs[idx]);
                else selected = idx;
            }

            int ty = ry + (UI_ROW - face_h(UI_FACE_BODY)) / 2;
            face_draw(&s, UI_PAD * 2, ty, songs[idx].name,
                      idx == selected ? t.fg : t.dim, UI_FACE_BODY);

            char size[24];
            int n = utoa(songs[idx].size / 1024, size);
            size[n++] = ' '; size[n++] = 'K'; size[n] = 0;
            face_draw(&s, w - UI_PAD * 2 - face_w(size, UI_FACE_SMALL), ty,
                      size, t.dim, UI_FACE_SMALL);
        }

        /* --- how far through ------------------------------------------------ */
        int py = h - foot + 6;
        round_rect(&s, UI_PAD, py, w - UI_PAD * 2, 8, 4, mix(t.bg, t.fg, 20));
        if (data_len) {
            int on = (int)((u64)(w - UI_PAD * 2) * data_done / data_len);
            if (on > 0) round_rect(&s, UI_PAD, py, on, 8, 4, t.accent);
        }

        ui_statusbar(&s, &t, w, h, status,
                     playing ? "playing" : (tune_at >= 0 ? "tune" : "ready"));
        win_commit(win);
        sleep_ms(playing ? 4 : 30);
    }

    if (fd >= 0) close(fd);
    win_close(win);
    exit(0);
}
