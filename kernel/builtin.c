/* Registering the built-in programs.
 *
 * builtin.S pastes the ELF files into the kernel image; this hands them to
 * the VFS, which lists them in /bin and serves reads straight out of the
 * kernel image.
 *
 * They are deliberately not copied onto the disk. If they were, the first
 * boot would write them out and every later boot would run the written
 * copies, so rebuilding the kernel would appear to change nothing. */
#include "builtin.h"
#include "vfs.h"
#include "printf.h"

extern const u8 builtin_hello_start[], builtin_hello_end[];
extern const u8 builtin_count_start[], builtin_count_end[];
extern const u8 builtin_wintest_start[], builtin_wintest_end[];
extern const u8 builtin_term_start[], builtin_term_end[];
extern const u8 builtin_spawntest_start[], builtin_spawntest_end[];
extern const u8 builtin_settings_start[], builtin_settings_end[];
extern const u8 builtin_paint_start[], builtin_paint_end[];
extern const u8 builtin_files_start[], builtin_files_end[];
extern const u8 builtin_notes_start[], builtin_notes_end[];
extern const u8 builtin_monitor_start[], builtin_monitor_end[];
extern const u8 builtin_calc_start[], builtin_calc_end[];
extern const u8 builtin_music_start[], builtin_music_end[];
extern const u8 builtin_browser_start[], builtin_browser_end[];

typedef struct {
    const char *name;
    const u8   *start, *end;
} program_t;

/* No extension. A program is a file in /bin; there is nothing else it
   could be, and "paint" reads better than "paint.elf" everywhere it
   appears. */
static const program_t PROGRAMS[] = {
    { "hello",     builtin_hello_start,     builtin_hello_end     },
    { "count",     builtin_count_start,     builtin_count_end     },
    { "wintest",   builtin_wintest_start,   builtin_wintest_end   },
    { "term",      builtin_term_start,      builtin_term_end      },
    { "spawntest", builtin_spawntest_start, builtin_spawntest_end },
    { "settings",  builtin_settings_start,  builtin_settings_end  },
    { "paint",     builtin_paint_start,     builtin_paint_end     },
    { "files",     builtin_files_start,     builtin_files_end     },
    { "notes",     builtin_notes_start,     builtin_notes_end     },
    { "monitor",   builtin_monitor_start,   builtin_monitor_end   },
    { "calc",      builtin_calc_start,      builtin_calc_end      },
    { "music",     builtin_music_start,     builtin_music_end     },
    { "browser",   builtin_browser_start,   builtin_browser_end   },
};

#define N_PROGRAMS (sizeof(PROGRAMS) / sizeof(PROGRAMS[0]))

u32 builtin_count_programs(void) { return N_PROGRAMS; }

void builtin_install(void) {
    for (u32 i = 0; i < N_PROGRAMS; i++) {
        const program_t *p = &PROGRAMS[i];
        vfs_add_builtin(p->name, p->start, (u32)(p->end - p->start));
    }
}
