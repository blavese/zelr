/* Where a program using this libc begins.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * sdk/zelr.h carries an entry point of its own, so that a program built
 * from one file needs nothing beside the header. A program built from
 * several files needs exactly one of them to have it, and this is that one:
 * every file here is compiled with ZELR_NO_START set, including this one,
 * and the _start below is the only definition in the program.
 *
 * What it adds over the header's version is the part a libc owes a program
 * before main runs and after it returns: stdout exists, and anything
 * written to it goes out even when main ends with a return rather than a
 * call to exit.
 */
/* The build script sets both of these; they are here so that this
   file is also correct when compiled on its own. */
#ifndef ZELR_NO_SUGAR
#define ZELR_NO_SUGAR 1
#endif
#ifndef ZELR_NO_START
#define ZELR_NO_START 1
#endif
#include "zelr.h"

extern int main(int argc, char **argv);
void _zstdio_start(void);
_Noreturn void exit(int status);

/* The stack the kernel built: argc, then argv, then a null, then an empty
   environment. Taken apart in C rather than in the assembly below, because
   the assembly's one job is to find the stack and it should not also be
   the place the calling convention is written down. */
_Noreturn void _zstart(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);

    _zstdio_start();
    exit(main(argc, argv));
}

__attribute__((naked, section(".text._start")))
void _start(void) {
    __asm__ volatile(
        "xorl %ebp, %ebp\n"                      /* the frame chain ends */
        "movq %rsp, %rdi\n"                      /* where the words are */
        "andq $-16, %rsp\n"                      /* what a call is promised */
        "call _zstart\n"
        "1: jmp 1b\n");                          /* _zstart does not return */
}
