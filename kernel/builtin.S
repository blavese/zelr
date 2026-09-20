/* The programs that ship inside the kernel image.
 *
 * These are ordinary ELF executables built by userland/build.sh, pasted in
 * here whole. Nothing about them is special: the kernel loads them through
 * the same ELF loader that reads a program off the disk. Shipping them this
 * way just means a fresh install has something to run before anyone has put
 * a file on a disk.
 *
 * The paths are relative to the repository root, which is where build.sh
 * runs the compiler from. */
.section .rodata
.balign 4096
.global trampoline_start
.global trampoline_end
trampoline_start:
.incbin "build/trampoline.bin"
trampoline_end:

.balign 4

.global builtin_hello_start
.global builtin_hello_end
builtin_hello_start:
.incbin "build/user/hello.elf"
builtin_hello_end:

.balign 4
.global builtin_count_start
.global builtin_count_end
builtin_count_start:
.incbin "build/user/count.elf"
builtin_count_end:

.balign 4
.global builtin_fptest_start
.global builtin_fptest_end
builtin_fptest_start:
.incbin "build/user/fptest.elf"
builtin_fptest_end:

.balign 4
.global builtin_alloctest_start
.global builtin_alloctest_end
builtin_alloctest_start:
.incbin "build/user/alloctest.elf"
builtin_alloctest_end:

.balign 4
.global builtin_jstest_start
.global builtin_jstest_end
builtin_jstest_start:
.incbin "build/user/jstest.elf"
builtin_jstest_end:

.balign 4
.global builtin_forktest_start
.global builtin_forktest_end
builtin_forktest_start:
.incbin "build/user/forktest.elf"
builtin_forktest_end:

.balign 4
.global builtin_wintest_start
.global builtin_wintest_end
builtin_wintest_start:
.incbin "build/user/wintest.elf"
builtin_wintest_end:

.balign 4
.global builtin_term_start
.global builtin_term_end
builtin_term_start:
.incbin "build/user/term.elf"
builtin_term_end:

.balign 4
.global builtin_spawntest_start
.global builtin_spawntest_end
builtin_spawntest_start:
.incbin "build/user/spawntest.elf"
builtin_spawntest_end:

.balign 4
.global builtin_settings_start
.global builtin_settings_end
builtin_settings_start:
.incbin "build/user/settings.elf"
builtin_settings_end:

.balign 4
.global builtin_paint_start
.global builtin_paint_end
builtin_paint_start:
.incbin "build/user/paint.elf"
builtin_paint_end:

.balign 4
.global builtin_files_start
.global builtin_files_end
builtin_files_start:
.incbin "build/user/files.elf"
builtin_files_end:

.balign 4
.global builtin_calc_start
.global builtin_calc_end
builtin_calc_start:
.incbin "build/user/calc.elf"
builtin_calc_end:

.balign 4
.global builtin_music_start
.global builtin_music_end
builtin_music_start:
.incbin "build/user/music.elf"
builtin_music_end:

.balign 4
.global builtin_monitor_start
.global builtin_monitor_end
builtin_monitor_start:
.incbin "build/user/monitor.elf"
builtin_monitor_end:

.balign 4
.global builtin_notes_start
.global builtin_notes_end
builtin_notes_start:
.incbin "build/user/notes.elf"
builtin_notes_end:

.balign 4
.global builtin_browser_start
.global builtin_browser_end
builtin_browser_start:
.incbin "build/user/browser.elf"
builtin_browser_end:

.balign 4
.global builtin_sh_start
.global builtin_sh_end
builtin_sh_start:
.incbin "build/user/sh.elf"
builtin_sh_end:

.balign 4
.global builtin_echo_start
.global builtin_echo_end
builtin_echo_start:
.incbin "build/user/echo.elf"
builtin_echo_end:

.balign 4
.global builtin_cat_start
.global builtin_cat_end
builtin_cat_start:
.incbin "build/user/cat.elf"
builtin_cat_end:

.balign 4
.global builtin_ls_start
.global builtin_ls_end
builtin_ls_start:
.incbin "build/user/ls.elf"
builtin_ls_end:

.balign 4
.global builtin_wc_start
.global builtin_wc_end
builtin_wc_start:
.incbin "build/user/wc.elf"
builtin_wc_end:

.balign 4
.global builtin_grep_start
.global builtin_grep_end
builtin_grep_start:
.incbin "build/user/grep.elf"
builtin_grep_end:

.balign 4
.global builtin_fdtest_start
.global builtin_fdtest_end
builtin_fdtest_start:
.incbin "build/user/fdtest.elf"
builtin_fdtest_end:

.balign 4
.global builtin_pagetest_start
.global builtin_pagetest_end
builtin_pagetest_start:
.incbin "build/user/pagetest.elf"
builtin_pagetest_end:

.balign 4
.global builtin_spin_start
.global builtin_spin_end
builtin_spin_start:
.incbin "build/user/spin.elf"
builtin_spin_end:

.balign 4
.global builtin_ps_start
.global builtin_ps_end
builtin_ps_start:
.incbin "build/user/ps.elf"
builtin_ps_end:

.balign 4
.global builtin_pngtest_start
.global builtin_pngtest_end
builtin_pngtest_start:
.incbin "build/user/pngtest.elf"
builtin_pngtest_end:

.balign 4
.global builtin_jpegtest_start
.global builtin_jpegtest_end
builtin_jpegtest_start:
.incbin "build/user/jpegtest.elf"
builtin_jpegtest_end:

.balign 4
.global builtin_svgtest_start
.global builtin_svgtest_end
builtin_svgtest_start:
.incbin "build/user/svgtest.elf"
builtin_svgtest_end:

.balign 4
.global builtin_layouttest_start
.global builtin_layouttest_end
builtin_layouttest_start:
.incbin "build/user/layouttest.elf"
builtin_layouttest_end:
