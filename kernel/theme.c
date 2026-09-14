/* The desktop's appearance, and the file it is kept in.
 *
 * The settings program runs in ring 3 and cannot reach into the window
 * manager, so the two communicate the way two programs normally would: one
 * writes a file and the other reads it. The window manager re-reads it a few
 * times a second, which is cheap for a file this size and means a change
 * shows up while the settings window is still open.
 *
 * The format is deliberately plain text. Someone who does not like any of
 * the presets can set a colour with the shell's `write` command. */
#include "theme.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "fb.h"

static theme_t current;

/* Six presets, differing only in accent and background temperature. The rest
   of the palette is derived, so a new accent never produces unreadable text. */
static const struct {
    const char *name;
    u32 accent, desktop, surface;
} PRESETS[THEME_PRESETS] = {
    { "teal",   RGB(0x2C, 0xC7, 0xA0), RGB(0x12, 0x18, 0x1E), RGB(0x1A, 0x22, 0x2A) },
    { "indigo", RGB(0x6E, 0x8A, 0xE8), RGB(0x14, 0x16, 0x22), RGB(0x1C, 0x1F, 0x2E) },
    { "amber",  RGB(0xE0, 0xA0, 0x3C), RGB(0x1A, 0x16, 0x12), RGB(0x25, 0x20, 0x1A) },
    { "rose",   RGB(0xE0, 0x6A, 0x8C), RGB(0x1B, 0x13, 0x18), RGB(0x27, 0x1C, 0x22) },
    { "slate",  RGB(0x8A, 0x9B, 0xB0), RGB(0x14, 0x17, 0x1B), RGB(0x1E, 0x23, 0x29) },
    { "lime",   RGB(0x9A, 0xD1, 0x4A), RGB(0x14, 0x1A, 0x14), RGB(0x1D, 0x25, 0x1D) },
};

bool wallpaper_moves(wallpaper_t w) { return w == WALLPAPER_STARS; }

const char *theme_preset_name(int i) {
    if (i < 0 || i >= THEME_PRESETS) return "";
    return PRESETS[i].name;
}

u32 theme_preset_accent(int i) {
    if (i < 0 || i >= THEME_PRESETS) return 0;
    return PRESETS[i].accent;
}

void theme_apply_preset(int i) {
    if (i < 0 || i >= THEME_PRESETS) return;
    current.accent = PRESETS[i].accent;
    current.desktop = PRESETS[i].desktop;
    current.surface = PRESETS[i].surface;
}

int theme_current_preset(void) {
    for (int i = 0; i < THEME_PRESETS; i++)
        if (PRESETS[i].accent == current.accent) return i;
    return -1;                            /* a colour someone set by hand */
}

const theme_t *theme(void) { return &current; }

void theme_init(void) {
    theme_apply_preset(0);
    current.text = RGB(0xE2, 0xE9, 0xEE);
    current.text_dim = RGB(0x77, 0x86, 0x93);
    current.wallpaper = WALLPAPER_GRADIENT;
    current.corner = 8;
    current.shadows = true;
    current.animate = true;
    current.quirks = true;
    theme_reload();
}

/* --- the file ----------------------------------------------------------- */

static u32 parse_hex(const char *s) {
    u32 v = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        u32 d;
        if (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
    }
    return v;
}

static u32 parse_dec(const char *s) {
    u32 v = 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (u32)(s[i] - '0');
    return v;
}

static void apply(const char *key, const char *value) {
    if (!strcmp(key, "accent"))      current.accent = parse_hex(value);
    else if (!strcmp(key, "desktop")) current.desktop = parse_hex(value);
    else if (!strcmp(key, "surface")) current.surface = parse_hex(value);
    else if (!strcmp(key, "text"))    current.text = parse_hex(value);
    else if (!strcmp(key, "wallpaper")) {
        u32 v = parse_dec(value);
        current.wallpaper = (wallpaper_t)(v < WALLPAPER_COUNT ? v : 0);
    }
    else if (!strcmp(key, "corner"))  {
        u32 v = parse_dec(value);
        current.corner = (int)(v > 20 ? 20 : v);
    }
    else if (!strcmp(key, "shadows")) current.shadows = parse_dec(value) != 0;
    else if (!strcmp(key, "animate")) current.animate = parse_dec(value) != 0;
    else if (!strcmp(key, "quirks"))  current.quirks = parse_dec(value) != 0;
    else if (!strcmp(key, "preset"))  theme_apply_preset((int)parse_dec(value));
}

bool theme_reload(void) {
    char buf[512];
    int n = vfs_read(THEME_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = 0;

    theme_t before = current;

    /* "key value" a line at a time. A line the parser does not recognise is
       skipped rather than treated as an error, so a config written by a
       later version still loads. */
    int i = 0;
    while (i < n) {
        char key[32], value[32];
        int k = 0, v = 0;

        while (i < n && (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') i++; continue; }

        while (i < n && buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\r') {
            if (k < (int)sizeof(key) - 1) key[k++] = buf[i];
            i++;
        }
        key[k] = 0;
        while (i < n && buf[i] == ' ') i++;
        while (i < n && buf[i] != '\n' && buf[i] != '\r') {
            if (v < (int)sizeof(value) - 1) value[v++] = buf[i];
            i++;
        }
        value[v] = 0;

        if (k) apply(key, value);
    }

    return memcmp(&before, &current, sizeof(theme_t)) != 0;
}

static int put_hex(char *out, u32 v) {
    const char *hex = "0123456789abcdef";
    for (int i = 5; i >= 0; i--) out[5 - i] = hex[(v >> (i * 4)) & 0xF];
    return 6;
}

static int put_num(char *out, u32 v) {
    char tmp[12];
    int n = 0;
    if (!v) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

bool theme_save(void) {
    char out[512];
    int n = 0;

    const char *header = "# zelr desktop settings\n# colours are RRGGBB in hex\n";
    for (const char *p = header; *p; p++) out[n++] = *p;

    struct { const char *key; u32 value; bool hex; } fields[] = {
        { "accent",    current.accent,          true },
        { "desktop",   current.desktop,         true },
        { "surface",   current.surface,         true },
        { "text",      current.text,            true },
        { "wallpaper", (u32)current.wallpaper,  false },
        { "quirks",    (u32)(current.quirks ? 1 : 0), false },
        { "corner",    (u32)current.corner,     false },
        { "shadows",   current.shadows ? 1u : 0u, false },
        { "animate",   current.animate ? 1u : 0u, false },
    };

    for (u32 f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        for (const char *p = fields[f].key; *p; p++) out[n++] = *p;
        out[n++] = ' ';
        n += fields[f].hex ? put_hex(out + n, fields[f].value)
                           : put_num(out + n, fields[f].value);
        out[n++] = '\n';
    }

    return vfs_write(THEME_FILE, out, (u32)n);
}
