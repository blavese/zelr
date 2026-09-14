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
.global builtin_notes_start
.global builtin_notes_end
builtin_notes_start:
.incbin "build/user/notes.elf"
builtin_notes_end:
