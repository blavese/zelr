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
 *
 * And AAC, which is what most music bought or ripped now is: an .m4a (an
 * MP4 holding an AAC track) is read whole and its frames found from its own
 * sample tables (mp4.h, mp4_index); an .aac is ADTS, the frames one after
 * another with a header each. Either is decoded by the stream player's
 * decoder (aac.h), 1024 frames at a time as they are needed, and stepped to
 * the card's rate as a WAV file's samples are.
 */
#include "zelr.h"
#include "ui.h"
#include "alloc.h"
#include "aac.h"
#include "mp4.h"

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

/* The AAC file playing, when it is one: all of it, where each frame is (an
   .m4a's) or where the next header is (ADTS), the decoder, and the last
   frame decoded. */
#define AAC_FILE_MAX (64 * 1024 * 1024)
static int   is_aac;
static u8   *aac_file;
static u32   aac_file_len;
static mp4_entry *aac_frames;
static int   aac_nframes, aac_next;
static u32   aac_adts_at;
static aac_dec aac_d;
static short aac_pcm[AAC_FRAME * AAC_MAX_CH];
static u32   aac_have;                 /* frames of it in aac_pcm */

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

static int ends_in(const char *name, const char *ext) {
    int n = 0, e = 0;
    while (name[n]) n++;
    while (ext[e]) e++;
    if (n <= e) return 0;
    for (int i = 0; i < e; i++) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ext[i]) return 0;
    }
    return 1;
}

static int is_wav(const char *name) { return ends_in(name, ".wav"); }
static int is_aac_name(const char *name) { return ends_in(name, ".m4a") || ends_in(name, ".aac"); }

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
        if (st.is_dir || !(is_wav(st.name) || is_aac_name(st.name))) continue;
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
static void aac_close_file(void) {
    if (aac_file) free(aac_file);
    if (aac_frames) free(aac_frames);
    aac_file = 0;
    aac_frames = 0;
    aac_file_len = 0;
    aac_nframes = aac_next = 0;
    aac_have = 0;
    is_aac = 0;
}

/* The one track of an .m4a that is AAC: low complexity, or HE-AAC, whose
   low complexity part is played (at its own rate, without the top half the
   extension would add). */
static const mp4_track *aac_track(const mp4_init *m) {
    for (int i = 0; i < m->n; i++)
        if (m->t[i].kind == MP4_AUDIO && (m->t[i].aot == 2 || m->t[i].aot == 5 || m->t[i].aot == 29))
            return &m->t[i];
    return 0;
}

static int open_aac(const song_t *s) {
    zelr_stat st;
    if (stat(s->path, &st) < 0 || st.is_dir) { say("cannot open it"); return 0; }
    if (st.size > AAC_FILE_MAX) { say("too long: 64 MB at most"); return 0; }
    aac_file = (u8 *)malloc(st.size + 1);
    if (!aac_file) { say("not enough memory for it"); return 0; }
    if (slurp(s->path, (char *)aac_file, (int)st.size + 1) != (int)st.size) {
        say("cannot read it");
        aac_close_file();
        return 0;
    }
    aac_file_len = st.size;

    int sfi, chans;
    if (ends_in(s->name, ".aac")) {
        aac_adts h;
        if (!aac_adts_read(aac_file, (int)aac_file_len, &h)) { say("not an aac file this can read"); aac_close_file(); return 0; }
        sfi = h.sfi;
        chans = h.channels;
        aac_adts_at = 0;
    } else {
        static mp4_init init;
        if (mp4_parse_init(aac_file, aac_file_len, &init) < 0) { say("not an m4a this can read"); aac_close_file(); return 0; }
        const mp4_track *t = aac_track(&init);
        if (!t) { say("it holds no aac sound"); aac_close_file(); return 0; }
        aac_nframes = mp4_index(aac_file, t, &aac_frames, &init);
        if (aac_nframes <= 0) { say(aac_nframes < 0 ? init.why : "there is nothing in it"); aac_close_file(); return 0; }
        sfi = t->sfi;
        chans = t->channels ? t->channels : 2;
    }
    if (!aac_open(&aac_d, sfi, chans)) {
        say(chans > AAC_MAX_CH ? "only mono and stereo are played" : "its rate is not one this can play");
        aac_close_file();
        return 0;
    }
    src_rate = (u32)AAC_RATES[aac_d.rate_idx].rate;
    src_chans = (u32)aac_d.channels;
    src_bits = 16;
    frac = 0;
    step = (u32)(((u64)src_rate << 16) / (dev_rate ? dev_rate : 48000));
    is_aac = 1;

    char msg[96];
    int n = 0;
    for (const char *p = "AAC, "; *p; p++) msg[n++] = *p;
    n += utoa(src_rate, msg + n);
    for (const char *p = " Hz, "; *p; p++) msg[n++] = *p;
    for (const char *p = src_chans == 1 ? "mono" : "stereo"; *p; p++) msg[n++] = *p;
    msg[n] = 0;
    say(msg);
    playing = 1;
    return 1;
}

/* The next frame decoded into aac_pcm; 0 when there is none left. A frame
   that will not decode is passed over, as the stream player passes one. */
static int aac_next_frame(void) {
    for (;;) {
        const u8 *p;
        int n;
        if (aac_frames) {
            if (aac_next >= aac_nframes) return 0;
            mp4_entry *e = &aac_frames[aac_next++];
            if (e->off < 0 || e->off + e->size > (long long)aac_file_len) continue;
            p = aac_file + e->off;
            n = (int)e->size;
        } else {
            aac_adts h;
            if (aac_adts_at >= aac_file_len
                || !aac_adts_read(aac_file + aac_adts_at, (int)(aac_file_len - aac_adts_at), &h)
                || aac_adts_at + (u32)h.frame_len > aac_file_len) return 0;
            p = aac_file + aac_adts_at + h.header_len;
            n = h.frame_len - h.header_len;
            aac_adts_at += (u32)h.frame_len;
        }
        if (aac_decode(&aac_d, p, n, aac_pcm) == AAC_FRAME) { aac_have = AAC_FRAME; return 1; }
    }
}

static int open_wav(const song_t *s) {
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

/* Lets go of whatever was playing and opens what was chosen, saying on the
   console how that went, as the status line does: a harness cannot read
   the window. */
static int open_song(const song_t *s) {
    if (fd >= 0) { close(fd); fd = -1; }
    playing = 0;
    aac_close_file();
    data_len = data_done = 0;
    int ok = is_aac_name(s->name) ? open_aac(s) : open_wav(s);
    puts(ok ? "music: playing " : "music: cannot play ");
    puts(s->name);
    puts(ok ? ", " : ": ");
    puts(status);
    puts("\n");
    return ok;
}

/* Whether the card has a third of a second queued. Its ring holds more than
   a second (for the stream player, which must not run dry while it fetches),
   and what is in it plays whatever happens here: Stop has to be heard to
   stop, so this keeps no more than a third of a second there. */
static int card_full(void) {
    zelr_sound snd;
    return sound_info(&snd) == 0 && snd.queued > dev_rate / 3;
}

/* One block: read source frames, step through them at the ratio, write the
   device's own rate out. Returns 0 when the file is finished. */
static int pump_aac(void) {
    if (card_full()) return 1;
    u32 made = 0, pos = frac;
    while (made < CHUNK) {
        u32 i = pos >> 16;
        if (i >= aac_have) {
            /* This frame is used up: the position carries over into the
               next, so the step stays even across the join. */
            pos -= aac_have << 16;
            if (!aac_next_frame()) { aac_have = 0; break; }
            continue;
        }
        short l = aac_pcm[i * src_chans];
        short r = src_chans > 1 ? aac_pcm[i * src_chans + 1] : l;
        out[made * 2] = l;
        out[made * 2 + 1] = r;
        made++;
        pos += step;
    }
    frac = pos;
    /* How far through, for the bar: frames of an .m4a, bytes of ADTS. */
    data_len = aac_frames ? (u32)aac_nframes : aac_file_len;
    data_done = aac_frames ? (u32)aac_next : aac_adts_at;
    if (made) sound_write(out, (int)made);
    return made > 0;
}

static int pump(void) {
    if (!playing) return 0;
    if (is_aac) return pump_aac();
    if (fd < 0) return 0;

    u32 src_frame = src_chans * (src_bits / 8);
    u32 left = data_len - data_done;
    if (!left) return 0;
    if (card_full()) return 1;

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
    if (card_full()) { tune_until = ticks() + 2; return; }

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

int main(int argc, char **argv) {
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
    const char *wanted = argc > 1 ? argv[1] : "";
    if (wanted[0]) {
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
            if (fd >= 0) close(fd);
            fd = -1;
            aac_close_file();
            say("finished");
            /* What the card still has to play of it, which is what Stop
               would have left playing: a third of a second (card_full).
               appcheck reads it here. */
            zelr_sound snd;
            if (sound_info(&snd) == 0 && dev_rate) {
                puts("music: finished with ");
                putn((int)((u64)snd.queued * 1000 / (u32)dev_rate));
                puts(" ms still to play\n");
            }
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
                aac_close_file();
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
                      "No .wav, .m4a or .aac files in /home, /music or /usb.",
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
        /* Playing, the card's buffer wants feeding every few milliseconds,
           so the loop keeps looking. Otherwise nothing changes until a key,
           the pointer or the next note of the tune: it looked every thirty
           milliseconds all the same, and drew and committed the window each
           time, thirty three frames a second of a player nobody was using. */
        if (playing) {
            sleep_ms(4);
        } else {
            if (tune_at >= 0) ui_due(tune_until);
            ui_wait(win);
        }
    }

    if (fd >= 0) close(fd);
    win_close(win);
    exit(0);
}
