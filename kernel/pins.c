/* The apps kept on the taskbar.
 *
 * A short list in a plain file, read once when the desktop starts and
 * written whenever it changes. The format is one line per app, the program
 * first and the name to show after it, so a person can read the file and
 * the shell's `write` command can edit it.
 *
 * Nothing here knows what an icon looks like or where the panel is. This
 * owns the list and the file; the window manager owns the drawing. */
#include "pins.h"
#include "vfs.h"
#include "string.h"

static pin_t list[PIN_MAX];
static int   count;

/* What a machine that has never been told otherwise starts with: the
   programs it ships, in the order the launcher lists them. */
static const pin_t DEFAULTS[] = {
    { "Terminal", "/bin/term" },
    { "Files",    "/bin/files" },
    { "Notes",    "/bin/notes" },
    { "Paint",    "/bin/paint" },
    { "Settings", "/bin/settings" },
};

int pins_count(void) { return count; }

const pin_t *pin_at(int i) {
    if (i < 0 || i >= count) return 0;
    return &list[i];
}

int pins_find(const char *path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < count; i++)
        if (!strcmp(list[i].path, path)) return i;
    return -1;
}

/* The last component of a path, for a line that named a program and no
   name to show for it. */
static const char *basename_of(const char *path) {
    const char *last = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') last = p + 1;
    return last;
}

static void append(const char *label, const char *path) {
    if (count >= PIN_MAX || !path || !path[0]) return;
    if (pins_find(path) >= 0) return;

    if (!label || !label[0]) label = basename_of(path);
    strncpy(list[count].label, label, PIN_LABEL - 1);
    list[count].label[PIN_LABEL - 1] = 0;
    strncpy(list[count].path, path, PIN_PATH - 1);
    list[count].path[PIN_PATH - 1] = 0;
    count++;
}

bool pins_add(const char *label, const char *path) {
    int before = count;
    append(label, path);
    if (count == before) return false;
    pins_save();
    return true;
}

void pins_remove(int i) {
    if (i < 0 || i >= count) return;
    for (int k = i; k < count - 1; k++) list[k] = list[k + 1];
    count--;
    pins_save();
}

void pins_move(int from, int to) {
    if (from < 0 || from >= count) return;
    if (to < 0) to = 0;
    if (to >= count) to = count - 1;
    if (to == from) return;

    pin_t moved = list[from];
    if (to > from) for (int k = from; k < to; k++) list[k] = list[k + 1];
    else           for (int k = from; k > to; k--) list[k] = list[k - 1];
    list[to] = moved;
}

void pins_init(void) {
    char buf[512];
    count = 0;

    int n = vfs_read(PIN_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) {
        for (u32 i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++)
            append(DEFAULTS[i].label, DEFAULTS[i].path);
        return;
    }
    buf[n] = 0;

    /* "path name" a line at a time. The file always carries a header line,
       which is what lets a list somebody emptied be told apart from no list
       at all: the first would be an empty file and read as never set. */
    int i = 0;
    while (i < n) {
        while (i < n && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') i++; continue; }

        char path[PIN_PATH], label[PIN_LABEL];
        int k = 0;
        while (i < n && buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\r') {
            if (k < PIN_PATH - 1) path[k++] = buf[i];
            i++;
        }
        path[k] = 0;

        while (i < n && buf[i] == ' ') i++;
        k = 0;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') {
            if (k < PIN_LABEL - 1) label[k++] = buf[i];
            i++;
        }
        label[k] = 0;

        append(label, path);
    }
}

bool pins_reload(void) {
    pin_t before[PIN_MAX];
    int had = count;
    memcpy(before, list, sizeof(before));

    pins_init();
    if (had != count) return true;
    return memcmp(before, list, sizeof(pin_t) * (u32)count) != 0;
}

bool pins_save(void) {
    char out[512];
    int n = 0;

    const char *header = "# zelr taskbar, one program a line\n";
    for (const char *p = header; *p; p++) out[n] = *p, n++;

    for (int i = 0; i < count; i++) {
        for (const char *p = list[i].path; *p && n < (int)sizeof(out) - 2; p++)
            out[n] = *p, n++;
        out[n] = ' '; n++;
        for (const char *p = list[i].label; *p && n < (int)sizeof(out) - 2; p++)
            out[n] = *p, n++;
        out[n] = '\n'; n++;
    }

    return vfs_write(PIN_FILE, out, (u32)n);
}
