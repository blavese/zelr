/* Turning the floating point registers on, and keeping them apart.
 *
 * See include/fpu.h for why this exists at all. What is here is the four
 * instructions that enable the unit and the two that move its state.
 */
#include "fpu.h"
#include "string.h"

/* CR0 */
#define CR0_MP (1u << 1)      /* monitor coprocessor */
#define CR0_EM (1u << 2)      /* emulate: set means "there is no unit" */
#define CR0_TS (1u << 3)      /* task switched */

/* CR4 */
#define CR4_OSFXSR     (1u << 9)   /* the OS will use FXSAVE, so SSE is legal */
#define CR4_OSXMMEXCPT (1u << 10)  /* and will handle SSE's own exceptions */

static bool ready;

static inline u64 read_cr0(void) {
    u64 v;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(u64 v) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(v) : "memory");
}

static inline u64 read_cr4(void) {
    u64 v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(u64 v) {
    __asm__ volatile ("mov %0, %%cr4" :: "r"(v) : "memory");
}

void fpu_init(void) {
    u64 cr0 = read_cr0();

    /* EM set means every floating point instruction traps so software can
       pretend to be a coprocessor. That is the state the machine starts in
       and it is the one to leave: there is a real unit here.

       MP set means the processor will raise the "device not available"
       fault on WAIT when the state has been switched away. Left on because
       it costs nothing and TS is cleared below anyway. */
    cr0 &= ~(u64)CR0_EM;
    cr0 |= CR0_MP;
    cr0 &= ~(u64)CR0_TS;
    write_cr0(cr0);

    /* And the two bits that say the operating system is prepared. Without
       OSFXSR every SSE instruction is an invalid opcode, which is what this
       whole project had been avoiding by compiling without them. */
    u64 cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(cr4);

    /* A known state rather than whatever the firmware left. */
    __asm__ volatile ("fninit");

    ready = true;
}

bool fpu_ready(void) { return ready; }

void fpu_save(void *area) {
    if (!ready) return;
    __asm__ volatile ("fxsave (%0)" :: "r"(area) : "memory");
}

void fpu_restore(const void *area) {
    if (!ready) return;
    __asm__ volatile ("fxrstor (%0)" :: "r"(area) : "memory");
}

/* What a task that has never run should find in its registers.
 *
 * Built by hand rather than by initialising the unit and saving it, because
 * that would mean disturbing the registers of whichever task happens to be
 * running in order to make a task that is not. The three fields that matter
 * are the x87 control word, its tag word, and MXCSR; everything else is
 * zero, which is what a register with nothing in it holds. */
void fpu_blank(void *area) {
    u8 *a = (u8 *)area;
    memset(a, 0, FPU_AREA);

    /* 0x037F: all exceptions masked, round to nearest, 64 bit precision.
       A program that divides by zero gets an infinity and carries on, which
       is what every language above this expects; unmasking would turn it
       into a fault nobody here is prepared to handle. */
    a[0] = 0x7F;
    a[1] = 0x03;

    /* Every x87 register empty. */
    a[4] = 0xFF;

    /* MXCSR at offset 24: the same masks for the vector side. 0x1F80. */
    a[24] = 0x80;
    a[25] = 0x1F;

    /* And the mask of which MXCSR bits are writable, at 28. The processor
       refuses an FXRSTOR whose reserved bits are set, so this says which
       ones this state is making a claim about. */
    a[28] = 0xFF;
    a[29] = 0xFF;
}
