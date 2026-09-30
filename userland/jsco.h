/* A function that stops in the middle and carries on later.
 *
 * A generator stops at every yield, and an async function at every await,
 * with its place in the middle of an expression kept for when it carries on:
 * `f(a, await b, c)` has worked out f and a and has not started on c. The
 * interpreter is a tree walker, and its place in a tree is the C stack of
 * calls that got it there; there is nowhere else it is written down. So a
 * function that stops keeps its C stack, and the way to keep one is to give
 * the function a stack of its own and switch to it and away from it.
 *
 * The alternative was to write the evaluator a second time, as a machine
 * with an explicit stack that can be put down and picked up, for every kind
 * of node a yield or an await can sit inside. That is a second evaluator to
 * keep in step with the first for ever. A switch is a dozen instructions.
 *
 * What makes the stacks small is that a suspended function's stack only ever
 * holds the walk over its own body. Anything it calls -- a method, a getter,
 * a native -- is handed back to the main stack to run (co_call_out), and the
 * answer handed back in, so however deep the work a generator asks for goes,
 * its own stack is as deep as the text of its body and no deeper. A page
 * with a hundred suspended async functions holds a hundred small stacks, not
 * a hundred deep ones, and the stacks count against the page's memory.
 *
 * Two versions of the switch, because the same code runs in two places:
 * zelr's ring 3, which calls in the System V way, and the Windows host build
 * (tools/host), which calls in Microsoft's -- with rdi, rsi and xmm6 to
 * xmm15 kept across a call, and a thread block that says where the stack is.
 */
#pragma once
#include "js.h"

/* A stack for a suspended function. The last part of it is kept free of
   anything the interpreter does, for the natives, the host's hooks and the
   switch itself: the interpreter refuses to go deeper than the margin. */
#define JS_CO_STACK  (64u * 1024)
#define JS_CO_MARGIN (16u * 1024)

void zelr_jsco_swap(void **save_sp, void *to_sp);
void zelr_jsco_boot(void);

#if defined(_WIN32)
/* rcx: where to save this stack's pointer; rdx: the stack to go to. The
   callee-saved registers, xmm6 to xmm15 and the thread block's idea of the
   stack's bounds are pushed on the stack being left and popped off the one
   being gone to. */
__asm__(
    ".text\n"
    ".globl zelr_jsco_swap\n"
    "zelr_jsco_swap:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq $176, %rsp\n"
    "    movdqu %xmm6, 0(%rsp)\n"
    "    movdqu %xmm7, 16(%rsp)\n"
    "    movdqu %xmm8, 32(%rsp)\n"
    "    movdqu %xmm9, 48(%rsp)\n"
    "    movdqu %xmm10, 64(%rsp)\n"
    "    movdqu %xmm11, 80(%rsp)\n"
    "    movdqu %xmm12, 96(%rsp)\n"
    "    movdqu %xmm13, 112(%rsp)\n"
    "    movdqu %xmm14, 128(%rsp)\n"
    "    movdqu %xmm15, 144(%rsp)\n"
    "    movq %gs:8, %rax\n"
    "    movq %rax, 160(%rsp)\n"
    "    movq %gs:16, %rax\n"
    "    movq %rax, 168(%rsp)\n"
    "    movq %rsp, (%rcx)\n"
    "    movq %rdx, %rsp\n"
    "    movdqu 0(%rsp), %xmm6\n"
    "    movdqu 16(%rsp), %xmm7\n"
    "    movdqu 32(%rsp), %xmm8\n"
    "    movdqu 48(%rsp), %xmm9\n"
    "    movdqu 64(%rsp), %xmm10\n"
    "    movdqu 80(%rsp), %xmm11\n"
    "    movdqu 96(%rsp), %xmm12\n"
    "    movdqu 112(%rsp), %xmm13\n"
    "    movdqu 128(%rsp), %xmm14\n"
    "    movdqu 144(%rsp), %xmm15\n"
    "    movq 160(%rsp), %rax\n"
    "    movq %rax, %gs:8\n"
    "    movq 168(%rsp), %rax\n"
    "    movq %rax, %gs:16\n"
    "    addq $176, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rsi\n"
    "    popq %rdi\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    /* Where a new stack's first switch returns to: the entry in r13, its
       argument in r12, called with the shadow space the convention wants.
       The entry never returns; it switches away for the last time. */
    ".globl zelr_jsco_boot\n"
    "zelr_jsco_boot:\n"
    "    movq %r12, %rcx\n"
    "    subq $32, %rsp\n"
    "    callq *%r13\n"
    "    ud2\n"
);
#define JS_CO_FRAME (176 + 8 * 8)
#else
/* rdi: where to save this stack's pointer; rsi: the stack to go to. */
__asm__(
    ".text\n"
    ".globl zelr_jsco_swap\n"
    "zelr_jsco_swap:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rsp, (%rdi)\n"
    "    movq %rsi, %rsp\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbx\n"
    "    popq %rbp\n"
    "    ret\n"
    ".globl zelr_jsco_boot\n"
    "zelr_jsco_boot:\n"
    "    movq %r12, %rdi\n"
    "    callq *%r13\n"
    "    ud2\n"
);
#define JS_CO_FRAME (6 * 8)
#endif

/* What a suspended function is, and why it last gave the stack back. */
enum { CO_START = 0, CO_SUSPENDED, CO_RUNNING, CO_DONE };
enum { CO_GEN = 1, CO_ASYNC, CO_ASYNCGEN };
enum { CO_WHY_YIELD = 1, CO_WHY_AWAIT, CO_WHY_CALL, CO_WHY_END };
enum { CO_NEXT = 0, CO_THROW, CO_RETURN };

/* A request made to an async generator: next, throw or return, each with
   the promise its caller was given. */
typedef struct jareq {
    int mode;
    jval value;
    jobj *promise;
    struct jareq *next;
} jareq;

typedef struct jco {
    void  *sp;                /* its stack pointer, while it is suspended */
    void  *back_sp;           /* the main stack's, while it runs */
    u8    *stack;             /* malloc'd; 0 once it is finished */

    u8     state, kind, why, mode;
    jctx  *J;

    /* how it started */
    jobj  *fn;
    jval   this_val;
    jval  *args;
    int    argc;

    /* A value going in (with `mode`) or coming out (with `why`). */
    jval   value;

    /* An async function's promise, or for an async generator the requests
       waiting for it. */
    jobj  *promise;
    jareq *queue, *queue_tail;

    /* A call it asked the main stack to make for it. */
    jval   c_fn, c_this, c_newtarget;
    jval  *c_argv;
    int    c_argc, c_construct;
    jval   c_result;

    /* The main stack's state while this runs. */
    struct jco *saved_current;
    char  *saved_limit;

    /* Every one holding a stack, so the page's end can give them back. */
    struct jco *all_prev, *all_next;
} jco;

static void co_entry(void *arg);

/* A stack for it, laid out so the first switch to it lands in the entry. */
static int co_stack_new(jctx *J, jco *co) {
    if (J->allocated + JS_CO_STACK > J->mem_cap) {
        js_out_of_memory(J);
        return 0;
    }
    u8 *s = (u8 *)malloc(JS_CO_STACK);
    if (!s) { js_out_of_memory(J); return 0; }
    J->allocated += JS_CO_STACK;
    J->co_live++;
    co->stack = s;
    co->all_prev = 0;
    co->all_next = J->co_all;
    if (J->co_all) J->co_all->all_prev = co;
    J->co_all = co;

    u64 top = ((u64)(s + JS_CO_STACK)) & ~(u64)15;
    u64 *ret = (u64 *)(top - 24);                /* 8 mod 16, so the entry is aligned */
    ret[0] = (u64)zelr_jsco_boot;
    u64 *frame = (u64 *)((u8 *)ret - JS_CO_FRAME);
    volatile u8 *z = (volatile u8 *)frame;
    for (int i = 0; i < JS_CO_FRAME; i++) z[i] = 0;
#if defined(_WIN32)
    /* xmm6-15 (zero), then the stack's bounds for the thread block, then
       r15, r14, r13, r12, rsi, rdi, rbx, rbp. */
    frame[20] = top;                             /* StackBase: its top */
    frame[21] = (u64)s;                          /* StackLimit: its bottom */
    frame[22 + 2] = (u64)co_entry;               /* r13 */
    frame[22 + 3] = (u64)co;                     /* r12 */
#else
    /* r15, r14, r13, r12, rbx, rbp */
    frame[2] = (u64)co_entry;                    /* r13 */
    frame[3] = (u64)co;                          /* r12 */
#endif
    co->sp = frame;
    return 1;
}

static void co_stack_free(jctx *J, jco *co) {
    if (!co->stack) return;
    free(co->stack);
    co->stack = 0;
    if (co->all_prev) co->all_prev->all_next = co->all_next;
    else J->co_all = co->all_next;
    if (co->all_next) co->all_next->all_prev = co->all_prev;
    co->all_prev = co->all_next = 0;
    if (J->allocated >= JS_CO_STACK) J->allocated -= JS_CO_STACK;
    if (J->co_live) J->co_live--;
}

/* From the main stack into the function, until it gives the stack back. */
static void co_switch_in(jctx *J, jco *co) {
    co->saved_current = J->co_current;
    co->saved_limit = J->stack_limit;
    J->co_current = co;
    J->stack_limit = (char *)co->stack + JS_CO_MARGIN;
    co->state = CO_RUNNING;
    zelr_jsco_swap(&co->back_sp, co->sp);
    J->co_current = co->saved_current;
    J->stack_limit = co->saved_limit;
}

/* From the function back to whoever switched in, saying why. */
static void co_switch_out(jco *co, int why) {
    co->why = (u8)why;
    if (why != CO_WHY_CALL) co->state = why == CO_WHY_END ? CO_DONE : CO_SUSPENDED;
    zelr_jsco_swap(&co->sp, co->back_sp);
    co->state = CO_RUNNING;
}
