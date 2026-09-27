/* A program that knows nothing about zelr.
 *
 * Part of zelr's libc. Copyright (C) 2026 blavese.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every line here is standard C. There is no zelr.h, no system call, and
 * nothing that would not compile on any machine with a C compiler -- which
 * is the whole claim this directory makes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

static int by_value(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    printf("hello from a program with a libc\n");

    printf("argc %d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);

    char *copy = strdup("the quick brown fox");
    if (!copy) { fprintf(stderr, "out of memory\n"); return 1; }
    for (char *p = copy; *p; p++) *p = (char)toupper((unsigned char)*p);
    printf("%s (%zu characters)\n", copy, strlen(copy));
    free(copy);

    int n[] = { 42, 7, 19, 3, 88, 1, 56 };
    size_t count = sizeof(n) / sizeof(n[0]);
    qsort(n, count, sizeof(n[0]), by_value);
    printf("sorted:");
    for (size_t i = 0; i < count; i++) printf(" %d", n[i]);
    printf("\n");

    printf("%d %5d %-5d| %05d %+d\n", 7, 7, 7, 7, 7);
    printf("%x %X %o %c %s %p\n", 48879, 48879, 8, 'z', "text", (void *)0x1234);
    printf("%.3f %.0f %8.2f\n", 3.14159, 2.5, -1.5);
    printf("%s\n", strtol("  -123abc", NULL, 10) == -123 ? "strtol ok" : "strtol WRONG");

    printf("sqrt(2) = %.6f\n", sqrt(2.0));
    printf("pow(2,10) = %.0f\n", pow(2.0, 10.0));
    printf("sin(pi/6) = %.6f\n", sin(M_PI / 6));

    FILE *f = fopen("libctest.txt", "w");
    if (!f) { perror("libctest.txt"); return 1; }
    fprintf(f, "written by a foreign program\nsecond line\n");
    fclose(f);

    f = fopen("libctest.txt", "r");
    if (!f) { perror("libctest.txt"); return 1; }
    char line[128];
    while (fgets(line, sizeof(line), f)) printf("read back: %s", line);
    fclose(f);
    remove("libctest.txt");

    /* atan, and atan2 in the second quadrant. */
    printf("atan(1)*4 = %.6f\n", atan(1.0) * 4);
    printf("atan2(1,-1) = %.6f\n", atan2(1.0, -1.0));

    /* A buffer filled to the byte by fwrite, one more character after it,
       and a second file open beside it that must not notice. */
    FILE *a = fopen("fill.txt", "w");
    FILE *b = fopen("next.txt", "w");
    if (a && b) {
        static char block[4096];
        memset(block, 'a', sizeof(block));
        fwrite(block, 1, sizeof(block), a);
        fputc('!', a);
        fputs("the next file is intact\n", b);
    }
    if (a) fclose(a);
    if (b) fclose(b);
    b = fopen("next.txt", "r");
    if (b && fgets(line, sizeof(line), b)) printf("next: %s", line);
    else printf("next: missing\n");
    if (b) fclose(b);
    long filled = -1;
    a = fopen("fill.txt", "r");
    if (a) { fseek(a, 0, SEEK_END); filled = ftell(a); fclose(a); }
    printf("fill: %ld bytes\n", filled);
    remove("fill.txt");
    remove("next.txt");

    /* A seek from where the program is, after one byte of a file has been
       read and the buffer has read the rest; and where it is while writing,
       before anything has been flushed. */
    f = fopen("seek.txt", "w");
    if (f) {
        fputs("abc", f);
        printf("tell while writing: %ld\n", ftell(f));
        fputs("defghij", f);
        fclose(f);
    }
    f = fopen("seek.txt", "r");
    if (f) {
        fgetc(f);
        fseek(f, 2, SEEK_CUR);
        int c = fgetc(f);
        printf("seek from here: %c at %ld\n", c, ftell(f));
        fclose(f);
    }
    remove("seek.txt");

    /* A line typed at the program. */
    printf("type a line:\n");
    if (fgets(line, sizeof(line), stdin)) printf("stdin said: %s", line);
    else printf("stdin said nothing\n");

    /* One second by clock(), which the harness times with its own. */
    printf("clock start\n");
    clock_t c0 = clock();
    while (clock() - c0 < CLOCKS_PER_SEC) { }
    printf("clock done\n");

    printf("LIBC_HELLO_OK\n");
    return 0;
}
