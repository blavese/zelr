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
static char shown_why[96];          /* why there is no picture, when there is none */
static char said[160];

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

static void draw_picture(surface *s, int x0, int y0, int room_w, int room_h) {
    if (!pic.rgb) return;
    int k = scale16(room_w, room_h);
    int dw = pic.w * k / 16, dh = pic.h * k / 16;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = x0 + (room_w - dw) / 2, oy = y0 + (room_h - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = y * pic.h / dh;
        int py = oy + y;
        if (py < 0 || py >= s->h) continue;
        const u8 *src = pic.rgb + (u32)sy * pic.w * 3;
        u32 *dst = s->px + (u32)py * s->w;
        for (int x = 0; x < dw; x++) {
            int px = ox + x;
            if (px < 0 || px >= s->w) continue;
            const u8 *p = src + (x * pic.w / dw) * 3;
            dst[px] = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
        }
    }
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

        for (int ki = 0; ki < in.nkeys; ki++) {
            u32 k = in.keys[ki];
            if ((k == KEY_RIGHT || k == KEY_DOWN || k == ' ') && at + 1 < nnames) load(at + 1);
            else if ((k == KEY_LEFT || k == KEY_UP) && at > 0) load(at - 1);
            else if (k == KEY_HOME && nnames) load(0);
            else if (k == KEY_END && nnames) load(nnames - 1);
        }

        int bar = UI_ROW + 4;
        fill(&s, BACKDROP);
        draw_picture(&s, 0, 0, w, h - bar);
        if (!pic.rgb) {
            const char *msg = shown_why[0] ? shown_why : "no picture";
            face_draw(&s, (w - face_w(msg, UI_FACE_BODY)) / 2, (h - bar) / 2, msg, 0xc8ccd2, UI_FACE_BODY);
        }
        int k = scale16(w, h - bar);
        say_shown(k);

        char right[48];
        int n = 0;
        if (nnames) {
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
