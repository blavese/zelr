/* Does this machine actually have floating point?
 *
 * Every other program here could be compiled with the vector instructions
 * switched off and would not notice. This one cannot: every line of it is a
 * double, and if SSE is not enabled the first of them is an invalid opcode
 * and the program never prints anything at all.
 *
 * So the check is partly just that it runs. The rest is that the answers are
 * right, which is a different question: a unit that is on but left in the
 * firmware's rounding mode, or a task switch that does not put the registers
 * away, both produce a program that runs and lies.
 *
 * Printed as scaled integers rather than as decimals, because writing a
 * double out in decimal is its own piece of work and this is a test of the
 * arithmetic and not of the formatting. A value times a million, truncated,
 * says everything that needs saying about whether the fraction survived.
 */
#include "zelr.h"

int main(void);

__attribute__((section(".text._start"))) void _start(void) {
    exit(main());
}

static int failed;

/* The value times a million, which is where the fractional part shows. An
   integer machine pretending to have doubles gets these wrong by exactly the
   fraction it threw away. */
static void check(const char *what, double got, long long want_micros) {
    long long micros = (long long)(got * 1000000.0);
    int ok = micros == want_micros;
    puts(ok ? "  PASS  " : "  FAIL  ");
    puts(what);
    puts("  = ");

    /* Printed out of a copy. The first version of this negated `micros` to
       print the sign and then compared the negated value against the one it
       was given, so every negative answer printed PASS and complained about
       itself in the same line. */
    long long shown = micros < 0 ? -micros : micros;
    if (micros < 0) putc('-');
    putn((int)(shown / 1000000));
    putc('.');
    /* Six places, with the leading zeros a plain putn would drop. */
    int frac = (int)(shown % 1000000);
    for (int d = 100000; d >= 1; d /= 10) {
        putn(frac / d);
        frac %= d;
    }
    if (!ok) {
        puts("   (wanted ");
        putn((int)(want_micros / 1000000));
        puts(" and a fraction)");
        failed++;
    }
    putc('\n');
}

/* A square root by Newton's method, which needs real division to converge
   and is the shortest thing that is obviously wrong under integers. */
static double root(double x) {
    if (x <= 0) return 0;
    double g = x;
    for (int i = 0; i < 40; i++) g = (g + x / g) / 2.0;
    return g;
}

int main(void) {
    puts("floating point, in ring 3\n");

    /* The one every integer machine gets wrong: a third is not zero. */
    check("1 / 3", 1.0 / 3.0, 333333);
    check("2 / 3", 2.0 / 3.0, 666666);

    /* The one every floating point machine gets wrong, and should: a tenth
       is not representable in binary, so the answer is near and not exact,
       and a test that demanded exactness here would be testing the wrong
       thing. */
    check("0.1 + 0.2", 0.1 + 0.2, 300000);

    check("root of 2", root(2.0), 1414213);
    check("root of 10", root(10.0), 3162277);

    /* Something with a long chain of operations, so a register that was not
       preserved across a task switch has somewhere to show up. */
    {
        double sum = 0.0;
        for (int i = 1; i <= 1000; i++) sum += 1.0 / (double)i;
        check("the first 1000 reciprocals", sum, 7485470);
    }

    /* Negative, and a fraction smaller than one, which between them use the
       sign bit and the low end of the exponent. */
    check("-7 / 8", -7.0 / 8.0, -875000);
    check("1 / 1024", 1.0 / 1024.0, 976);

    /* And that a double really is wider than a float. This value needs more
       than 24 bits of significand to come back; on a float it does not. */
    {
        double big = 123456789.0;
        double back = (big + 1.0) - big;
        check("123456789 + 1 - 123456789", back, 1000000);
    }

    puts(failed ? "FPTEST_FAIL\n" : "FPTEST_PASS\n");
    return failed;
}
