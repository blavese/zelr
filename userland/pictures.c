/* Pictures: a picture viewer.
 *
 * The browser could show a PNG, a JPEG, a GIF or a WebP inside a page, and
 * nothing could show one that was a file on the disk: Files opened every
 * file it did not know in the text editor, and Print Screen saves PNGs. This
 * shows one, with the browser's own decoders, fitted to the window and never
 * enlarged past its own size, and the arrow keys walk the other pictures in
 * its folder in the order of their names.
 *
 * What it shows it also says on the console ("pictures: NAME, W by H"), a line
 * each time it changes, which is how a harness knows (piccheck reads the
 * window, the line says which picture it should be).
 *
 * S shows the folder as a slideshow: each picture for three seconds, the
 * first again after the last, until S again or any key that moves.
 *
 *   /bin/pictures [FILE | FOLDER]
 */
#include "zelr.h"
#include "ui.h"
#include "alloc.h"
#include "png.h"
#include "jpeg.h"
#include "gif.h"
#include "webp.h"

#define MAX_FILES 256
#define NAME_MAX  64
#define PATH_MAX  256
#define BACKDROP  0x16181c          /* behind a picture: a dark grey, as viewers have it */

static char folder[PATH_MAX] = "/home/pictures";
static char names[MAX_FILES][NAME_MAX];
static int  nnames;
static int  at = -1;                /* into names: the picture shown */

static picture pic;                 /* its pixels, or rgb 0 */

/* How it is looked at: fitted to the window (zoom16 0), or at a scale in
   sixteenths that + and - double and halve; where in it the middle of the
   window is, when it is bigger than the window (pan_x, pan_y, its own
   pixels); and a drag that moves it. */
static int zoom16;
static int pan_x, pan_y;
static int drag_x, drag_y, dragging;
static char shown_why[96];          /* why there is no picture, when there is none */
static char said[160];

#define SHOW_TICKS 300              /* a slideshow's picture: three seconds */
static int show_on, show_next;      /* a slideshow, and the tick of its next picture */

static void show_stop(void) {
    if (!show_on) return;
    show_on = 0;
    puts("pictures: slideshow stopped\n");
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int starts(const u8 *d, const char *s) {
    for (int i = 0; s[i]; i++) if (d[i] != (u8)s[i]) return 0;
    return 1;
}

static int ends_with(const char *name, const char *ext) {
    int n = strlen(name), e = strlen(ext);
    if (n <= e) return 0;
    for (int i = 0; i < e; i++) if (lower(name[n - e + i]) != ext[i]) return 0;
    return 1;
}

static int is_picture(const char *name) {
    return ends_with(name, ".png") || ends_with(name, ".jpg") || ends_with(name, ".jpeg")
        || ends_with(name, ".gif") || ends_with(name, ".webp");
}

static void join(char *out, const char *dir, const char *name) {
    int i = 0;
    for (; dir[i] && i < PATH_MAX - 2; i++) out[i] = dir[i];
    if (i > 1 && out[i - 1] == '/') i--;
    out[i++] = '/';
    for (int j = 0; name[j] && i < PATH_MAX - 1; j++) out[i++] = name[j];
    out[i] = 0;
}

static int name_before(const char *a, const char *b) {
    for (int i = 0;; i++) {
        int x = lower(a[i]), y = lower(b[i]);
        if (x != y || !x) return x < y;
    }
}

/* The pictures in the folder, by name. */
static void list_folder(void) {
    nnames = 0;
    zelr_stat st;
    for (int i = 0; nnames < MAX_FILES && readdir(folder, i, &st) == 1; i++) {
        if (st.is_dir || !is_picture(st.name)) continue;
        int k = nnames++;
        while (k > 0 && name_before(st.name, names[k - 1])) {
            memcpy(names[k], names[k - 1], NAME_MAX);
            k--;
        }
        strncpy(names[k], st.name, NAME_MAX - 1);
        names[k][NAME_MAX - 1] = 0;
    }
}

static void drop_picture(void) {
    if (pic.rgb) free(pic.rgb);
    pic.rgb = 0;
    pic.w = pic.h = 0;
    zoom16 = 0;
    pan_x = pan_y = 0;
}

/* W: this picture as the wallpaper. The desktop has no decoders of its own,
   so it is made the screen's size here, covering it (cut at the long sides
   rather than left with bars), and written as /cfg/wallpaper.raw: "ZWP1",
   the width and height, then a 0xRRGGBB pixel each. */
static int screen_size(int *w, int *h) {
    char buf[512];
    int n = slurp("/sys/screen", buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    *w = *h = 0;
    for (int i = 0; i < n; i++) {
        if (i && buf[i - 1] != '\n') continue;
        int *to = !strncmp(buf + i, "width ", 6) ? w : !strncmp(buf + i, "height ", 7) ? h : 0;
        if (!to) continue;
        int j = i;
        while (buf[j] && buf[j] != ' ') j++;
        while (buf[j] == ' ') j++;
        for (; buf[j] >= '0' && buf[j] <= '9'; j++) *to = *to * 10 + buf[j] - '0';
    }
    return *w > 0 && *h > 0;
}

static void set_wallpaper(void) {
    int sw, sh;
    if (!pic.rgb || !screen_size(&sw, &sh)) return;
    u32 bytes = 12 + (u32)sw * sh * 4;
    u8 *out = (u8 *)malloc(bytes);
    if (!out) { puts("pictures: not enough memory for a wallpaper\n"); return; }
    out[0] = 'Z'; out[1] = 'W'; out[2] = 'P'; out[3] = '1';
    *(u32 *)(out + 4) = (u32)sw;
    *(u32 *)(out + 8) = (u32)sh;
    /* Covering: the larger of the two scales, the rest cut evenly. */
    long long dw = sw, dh = (long long)pic.h * sw / pic.w;
    if (dh < sh) { dh = sh; dw = (long long)pic.w * sh / pic.h; }
    long long ox = (dw - sw) / 2, oy = (dh - sh) / 2;
    u32 *px = (u32 *)(out + 12);
    for (int y = 0; y < sh; y++) {
        int sy = (int)((y + oy) * pic.h / dh);
        if (sy >= pic.h) sy = pic.h - 1;
        for (int x = 0; x < sw; x++) {
            int sx = (int)((x + ox) * pic.w / dw);
            if (sx >= pic.w) sx = pic.w - 1;
            const u8 *p = pic.rgb + ((u32)sy * pic.w + sx) * 3;
            px[(u32)y * sw + x] = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
        }
    }
    int ok = spit("/cfg/wallpaper.raw", out, (int)bytes) == (int)bytes;
    free(out);
    puts(ok ? "pictures: the wallpaper is " : "pictures: the wallpaper could not be written for ");
    puts(at >= 0 && at < nnames ? names[at] : "this picture");
    puts("\n");
}

/* A quarter turn to the right, into new memory: what was the left edge is
   the top. */
static void rotate_right(void) {
    if (!pic.rgb) return;
    u8 *to = (u8 *)malloc((u32)pic.w * pic.h * 3);
    if (!to) return;
    int nw = pic.h, nh = pic.w;
    for (int y = 0; y < pic.h; y++)
        for (int x = 0; x < pic.w; x++) {
            const u8 *s = pic.rgb + ((u32)y * pic.w + x) * 3;
            u8 *d = to + ((u32)x * nw + (nw - 1 - y)) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    free(pic.rgb);
    pic.rgb = to;
    pic.w = nw;
    pic.h = nh;
    zoom16 = 0;
    pan_x = pan_y = 0;
}

/* The file's bytes, decoded by whichever of the four its first bytes say it
   is; the name's ending is only what put it in the list. */
static void load(int index) {
    drop_picture();
    shown_why[0] = 0;
    at = index;
    if (index < 0 || index >= nnames) { strcpy(shown_why, "no pictures here"); return; }
    char path[PATH_MAX];
    join(path, folder, names[index]);
    zelr_stat st;
    if (stat(path, &st) != 0 || st.is_dir) { strcpy(shown_why, "it is not there"); return; }
    u8 *data = (u8 *)malloc(st.size ? st.size : 1);
    if (!data) { strcpy(shown_why, "not enough memory to read it"); return; }
    int n = read_file(path, (char *)data, (int)st.size);
    int r = -1;
    if (n >= 12) {
        if (data[0] == 0x89 && data[1] == 'P') r = png_decode(data, n, &pic, BACKDROP);
        else if (data[0] == 0xFF && data[1] == 0xD8) r = jpeg_decode(data, n, &pic);
        else if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F') r = gif_decode(data, n, &pic, BACKDROP);
        else if (starts(data, "RIFF") && starts(data + 8, "WEBP")) r = webp_decode(data, n, &pic, BACKDROP);
    }
    free(data);
    if (r != 0 || !pic.rgb || pic.w <= 0 || pic.h <= 0) {
        drop_picture();
        strcpy(shown_why, n < 12 ? "it could not be read" : "it is not a picture this can show");
    }
}

/* The scale it is drawn at, in sixteenths: fitted to the room, and never
   past its own size (a small picture enlarged is a blur nobody asked for). */
static int scale16(int room_w, int room_h) {
    if (!pic.rgb || room_w <= 0 || room_h <= 0) return 16;
    int s = 16;
    if (pic.w * s > room_w * 16) s = room_w * 16 / pic.w;
    if (pic.h * s > room_h * 16) s = room_h * 16 / pic.h;
    return s < 1 ? 1 : s;
}

static int scale_now(int room_w, int room_h) { return zoom16 ? zoom16 : scale16(room_w, room_h); }

/* Where the picture's left (or top) edge goes: in the middle when it fits,
   else so that pan is at the middle, pan kept where the picture still
   covers the room. */
static int edge_at(int size, int room, int k, int *pan) {
    int d = size * k / 16;
    if (d <= room) return (room - d) / 2;
    int half = room * 8 / k;            /* half the room, in the picture's pixels */
    if (*pan < half) *pan = half;
    if (*pan > size - half) *pan = size - half;
    return room / 2 - *pan * k / 16;
}

static void draw_picture(surface *s, int x0, int y0, int room_w, int room_h) {
    if (!pic.rgb) return;
    int k = scale_now(room_w, room_h);
    int dw = pic.w * k / 16, dh = pic.h * k / 16;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = x0 + edge_at(pic.w, room_w, k, &pan_x), oy = y0 + edge_at(pic.h, room_h, k, &pan_y);
    /* Only the part inside the room is worked out: at eight times, a large
       picture is tens of thousands of pixels across. */
    int ya = oy > y0 ? oy : y0, yb = oy + dh < y0 + room_h ? oy + dh : y0 + room_h;
    int xa = ox > x0 ? ox : x0, xb = ox + dw < x0 + room_w ? ox + dw : x0 + room_w;
    if (yb > s->h) yb = s->h;
    if (xb > s->w) xb = s->w;
    for (int py = ya < 0 ? 0 : ya; py < yb; py++) {
        int sy = (int)((long long)(py - oy) * pic.h / dh);
        const u8 *src = pic.rgb + (u32)sy * pic.w * 3;
        u32 *dst = s->px + (u32)py * s->w;
        for (int px = xa < 0 ? 0 : xa; px < xb; px++) {
            const u8 *p = src + (int)((long long)(px - ox) * pic.w / dw) * 3;
            dst[px] = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
        }
    }
}

/* + and -: twice or half the size, about the middle of what is shown; back
   to fitted when that is no bigger. 0 fits it again. */
static void zoom_by(int up, int room_w, int room_h) {
    if (!pic.rgb) return;
    int k = scale_now(room_w, room_h), fit = scale16(room_w, room_h);
    if (!zoom16) { pan_x = pic.w / 2; pan_y = pic.h / 2; }
    int nk = up ? k * 2 : k / 2;
    if (nk > 128) nk = 128;             /* eight times */
    if (nk < 1) nk = 1;
    zoom16 = nk <= fit && !up ? 0 : nk;
}

static void say_shown(int k) {
    char line[160];
    int n = 0;
    const char *name = at >= 0 && at < nnames ? names[at] : folder;
    for (const char *p = name; *p && n < 100; p++) line[n++] = *p;
    line[n++] = ','; line[n++] = ' ';
    if (pic.rgb) {
        n += utoa((u32)pic.w, line + n);
        for (const char *p = " by "; *p; p++) line[n++] = *p;
        n += utoa((u32)pic.h, line + n);
        line[n++] = ','; line[n++] = ' ';
        n += utoa((u32)(k * 100 / 16), line + n);
        line[n++] = '%';
    } else {
        for (const char *p = shown_why; *p && n < 150; p++) line[n++] = *p;
    }
    line[n] = 0;
    if (strcmp(line, said)) {
        strcpy(said, line);
        puts("pictures: ");
        puts(line);
        puts("\n");
    }
}

int main(int argc, char **argv) {
    int win = win_create("Pictures", 720, 540);
    if (win < 0) exit(1);
    win_allow_resize(win);

    /* A file shows that file among its folder's pictures; a folder shows
       its first. */
    const char *wanted = argc > 1 ? argv[1] : "";
    char first[NAME_MAX] = "";
    if (wanted[0]) {
        zelr_stat st;
        if (stat(wanted, &st) == 0 && st.is_dir) {
            strncpy(folder, wanted, PATH_MAX - 1);
        } else {
            int cut = -1;
            for (int i = 0; wanted[i]; i++) if (wanted[i] == '/') cut = i;
            if (cut > 0) { memcpy(folder, wanted, cut); folder[cut] = 0; }
            else if (cut == 0) strcpy(folder, "/");
            strncpy(first, wanted + cut + 1, NAME_MAX - 1);
        }
    }
    list_folder();
    int start = 0;
    for (int i = 0; i < nnames; i++) if (first[0] && !strcmp(names[i], first)) start = i;
    load(nnames ? start : -1);

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

        int bar = UI_ROW + 4;
        int room_h = h - bar;
        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 k = in.keys[ki];
            if (k == 's' || k == 'S') {
                if (show_on) show_stop();
                else if (nnames > 1) {
                    show_on = 1;
                    show_next = ticks() + SHOW_TICKS;
                    puts("pictures: slideshow, every 3 seconds\n");
                }
                continue;
            }
            if (k == KEY_RIGHT || k == KEY_LEFT || k == KEY_UP || k == KEY_DOWN || k == ' '
                || k == KEY_HOME || k == KEY_END) show_stop();
            int kz = scale_now(w, room_h);
            int wide = pic.rgb && pic.w * kz / 16 > w, tall = pic.rgb && pic.h * kz / 16 > room_h;
            /* The arrows move about a picture bigger than the window, and
               go between pictures otherwise. */
            if (k == KEY_RIGHT && wide) pan_x += w * 4 / kz;
            else if (k == KEY_LEFT && wide) pan_x -= w * 4 / kz;
            else if (k == KEY_DOWN && tall) pan_y += room_h * 4 / kz;
            else if (k == KEY_UP && tall) pan_y -= room_h * 4 / kz;
            else if ((k == KEY_RIGHT || k == KEY_DOWN || k == ' ') && at + 1 < nnames) load(at + 1);
            else if ((k == KEY_LEFT || k == KEY_UP) && at > 0) load(at - 1);
            else if (k == KEY_HOME && nnames) load(0);
            else if (k == KEY_END && nnames) load(nnames - 1);
            else if (k == '+' || k == '=') zoom_by(1, w, room_h);
            else if (k == '-') zoom_by(0, w, room_h);
            else if (k == '0') { zoom16 = 0; }
            else if (k == 'r' || k == 'R') rotate_right();
            else if (k == 'w' || k == 'W') set_wallpaper();
        }
        /* The slideshow's next picture when its time comes, and the loop
           woken for it (ui_wait sleeps until then, not a second). */
        if (show_on && ticks() - show_next >= 0) {
            load(at + 1 < nnames ? at + 1 : 0);
            show_next = ticks() + SHOW_TICKS;
        }
        if (show_on) ui_due(show_next);

        /* A drag moves a picture bigger than the window. */
        if (in.pressed) { show_stop(); dragging = 1; drag_x = in.mx; drag_y = in.my; }
        if (dragging && in.down && pic.rgb) {
            int kz = scale_now(w, room_h);
            pan_x -= (in.mx - drag_x) * 16 / kz;
            pan_y -= (in.my - drag_y) * 16 / kz;
            drag_x = in.mx;
            drag_y = in.my;
        }
        if (!in.down) dragging = 0;

        fill(&s, BACKDROP);
        draw_picture(&s, 0, 0, w, h - bar);
        if (!pic.rgb) {
            const char *msg = shown_why[0] ? shown_why : "no picture";
            face_draw(&s, (w - face_w(msg, UI_FACE_BODY)) / 2, (h - bar) / 2, msg, 0xc8ccd2, UI_FACE_BODY);
        }
        int k = scale_now(w, h - bar);
        say_shown(k);

        char right[48];
        int n = 0;
        if (nnames) {
            if (show_on) for (const char *p = "slideshow, "; *p; p++) right[n++] = *p;
            n += utoa((u32)(at + 1), right + n);
            for (const char *p = " of "; *p; p++) right[n++] = *p;
            n += utoa((u32)nnames, right + n);
        }
        right[n] = 0;
        ui_statusbar(&s, &t, w, h, said, right);

        win_commit(win);
        ui_wait(win);
    }
    drop_picture();
    win_close(win);
    exit(0);
}
