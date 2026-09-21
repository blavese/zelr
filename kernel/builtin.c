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
#include "sysfs.h"
#include "printf.h"

extern const u8 builtin_hello_start[], builtin_hello_end[];
extern const u8 builtin_count_start[], builtin_count_end[];
extern const u8 builtin_fptest_start[], builtin_fptest_end[];
extern const u8 builtin_alloctest_start[], builtin_alloctest_end[];
extern const u8 builtin_jstest_start[], builtin_jstest_end[];
extern const u8 builtin_forktest_start[], builtin_forktest_end[];
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
extern const u8 builtin_sh_start[], builtin_sh_end[];
extern const u8 builtin_echo_start[], builtin_echo_end[];
extern const u8 builtin_cat_start[], builtin_cat_end[];
extern const u8 builtin_ls_start[], builtin_ls_end[];
extern const u8 builtin_wc_start[], builtin_wc_end[];
extern const u8 builtin_grep_start[], builtin_grep_end[];
extern const u8 builtin_fdtest_start[], builtin_fdtest_end[];
extern const u8 builtin_pagetest_start[], builtin_pagetest_end[];
extern const u8 builtin_spin_start[], builtin_spin_end[];
extern const u8 builtin_ps_start[], builtin_ps_end[];
extern const u8 builtin_pngtest_start[], builtin_pngtest_end[];
extern const u8 builtin_jpegtest_start[], builtin_jpegtest_end[];
extern const u8 builtin_svgtest_start[], builtin_svgtest_end[];
extern const u8 builtin_layouttest_start[], builtin_layouttest_end[];
extern const u8 builtin_wiretest_start[], builtin_wiretest_end[];
extern const u8 builtin_jsprobe_start[], builtin_jsprobe_end[];
extern const u8 builtin_blackjack_start[], builtin_blackjack_end[];
extern const u8 builtin_poker_start[], builtin_poker_end[];
extern const u8 builtin_cardtest_start[], builtin_cardtest_end[];
extern const u8 builtin_sleeptest_start[], builtin_sleeptest_end[];
extern const u8 builtin_halfdrawn_start[], builtin_halfdrawn_end[];
extern const u8 builtin_cowtest_start[], builtin_cowtest_end[];

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
    { "fptest",    builtin_fptest_start,    builtin_fptest_end    },
    { "alloctest", builtin_alloctest_start, builtin_alloctest_end },
    { "jstest",    builtin_jstest_start,    builtin_jstest_end    },
    { "forktest",  builtin_forktest_start,  builtin_forktest_end  },
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
    { "sh",        builtin_sh_start,        builtin_sh_end        },
    { "echo",      builtin_echo_start,      builtin_echo_end      },
    { "cat",       builtin_cat_start,       builtin_cat_end       },
    { "ls",        builtin_ls_start,        builtin_ls_end        },
    { "wc",        builtin_wc_start,        builtin_wc_end        },
    { "grep",      builtin_grep_start,      builtin_grep_end      },
    { "fdtest",    builtin_fdtest_start,    builtin_fdtest_end    },
    { "pagetest",  builtin_pagetest_start,  builtin_pagetest_end  },
    { "spin",      builtin_spin_start,      builtin_spin_end      },
    { "ps",        builtin_ps_start,        builtin_ps_end        },
    { "pngtest",   builtin_pngtest_start,   builtin_pngtest_end   },
    { "jpegtest",  builtin_jpegtest_start,  builtin_jpegtest_end  },
    { "svgtest",   builtin_svgtest_start,   builtin_svgtest_end   },
    { "layouttest", builtin_layouttest_start, builtin_layouttest_end },
    { "wiretest", builtin_wiretest_start, builtin_wiretest_end },
    { "jsprobe", builtin_jsprobe_start, builtin_jsprobe_end },
    { "blackjack", builtin_blackjack_start, builtin_blackjack_end },
    { "poker",   builtin_poker_start,   builtin_poker_end   },
    { "cardtest", builtin_cardtest_start, builtin_cardtest_end },
    { "sleeptest", builtin_sleeptest_start, builtin_sleeptest_end },
    { "halfdrawn", builtin_halfdrawn_start, builtin_halfdrawn_end },
    { "cowtest",  builtin_cowtest_start,  builtin_cowtest_end  },
};

#define N_PROGRAMS (sizeof(PROGRAMS) / sizeof(PROGRAMS[0]))

/* There has to be room for all of them. Adding a program used to be adding
   a line here, and if that line was the seventeenth the program was dropped
   without a word: the table it goes into had room for sixteen and said
   nothing when it was full. This is that failure turned into one the
   compiler refuses. */
_Static_assert(N_PROGRAMS <= SYSFS_MAX_PROGRAMS,
               "more built-in programs than /bin has room for; "
               "raise SYSFS_MAX_PROGRAMS in include/sysfs.h");

u32 builtin_count_programs(void) { return N_PROGRAMS; }

void builtin_install(void) {
    for (u32 i = 0; i < N_PROGRAMS; i++) {
        const program_t *p = &PROGRAMS[i];
        vfs_add_builtin(p->name, p->start, (u32)(p->end - p->start));
    }
}
