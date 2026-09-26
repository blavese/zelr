# Atlas 03 -- Tasks, Processes and System Calls

Source root: the repository root (copy of github.com/blavese/zelr @ main, two commits past v0.37.0, 2026-09-22).
This section covers the scheduler, task model, ELF loader, ring-3 process building, the int 0x80 syscall dispatch, file descriptors, pipes, signals, wait queues, the "about" window, and the two ABI-checking Python tools. Cross-references into paging/pmm/idt/smp/timer are traced where the process machinery depends on them.

---

## 1. Scope

Headers (include/):
| File | Lines | Role |
|---|---|---|
| `sched.h` | 217 | task_t struct, task states, scheduler + kernel-lock API, VMA_MAX, stack size |
| `syscall.h` | 226 | Syscall numbers (kernel side), shared structs (zelr_task_t etc.), NET_ERR_*, POWER_* |
| `user.h` | 88 | Ring-3 spawn entry points, sbrk/mmap API, user address-space layout constants |
| `elf.h` | 13 | ELF error codes, `elf_load` / `elf_error` prototypes |
| `fd.h` | 97 | FD model doc, fd_* API, POLL* bits, pollfd_t, FD_MAX |
| `pipe.h` | 53 | pipe_t struct, pipe_* API, PIPE_SIZE |
| `signal.h` | 98 | Signal numbers/dispositions, signal_* API, the "why in the scheduler / handler on return" design doc |
| `wait.h` | 36 | wait_on / wake_all / wake_one API + counters |
| `apps.h` | 4 | `app_about` / `app_about_forget` prototypes |

Kernel (kernel/):
| File | Lines | Role |
|---|---|---|
| `sched.c` | 596 | Round-robin preemptive scheduler, per-CPU current, idle tasks, kernel lock, fork/exec task-record half, reaping |
| `syscall.c` | 940 | int 0x80 dispatch table, every syscall handler, user pointer validation, socket table |
| `user.c` | 355 | ring-3 process building: stack/argv, sbrk heap, mmap/munmap, page-fault fill, ELF/flat/stub spawn |
| `elf.c` | 100 | ELF64 loader with full validation |
| `fd.c` | 489 | Open-file-description table + per-task fd arrays, console, files, pipes, dup/dup2, poll |
| `pipe.c` | 112 | Ring buffer + blocking read/write/close, EOF |
| `signal.c` | 244 | pending/disposition, deliver (handler frame), sigreturn validation, ctrl-C targeting, signal_end_task |
| `wait.c` | 72 | Sleep-on-address wait queue with deadline |
| `apps.c` | 64 | The kernel-drawn "about" info window |

Tools (tools/):
| File | Lines | Role |
|---|---|---|
| `abicheck.py` | 138 | Compares syscall numbers and shared struct layouts between include/syscall.h and sdk/zelr.h |
| `ring3check.py` | 90 | Boots QEMU once, runs 19 ring-3 test ELFs, checks each prints its PASS marker |

SDK (read to reconcile ABI): `sdk/zelr.h` (845 lines) -- user-side wrappers, `_start`, signal trampoline, struct mirrors.

---

## 2. Big picture

zelr is a single-kernel-lock, preemptive, round-robin multitasking OS. A **task** is one thread of execution with its own kernel stack; a **process** (Unix sense) is a task with `user=true` running in ring 3 in its own page directory (`dir`). There is no thread abstraction below a process -- `fork` is the only way to get another schedulable entity in a user program (sdk/README.md: "No threads. fork makes a process; there is nothing smaller").

Design decisions and their stated reasons:

- **One big kernel lock** (`sched.h:164-188`). A processor holds it "whenever it is not executing ring 3 code." Chosen as "the coarsest lock there is and it is the honest one to start with," because the alternative is 40 fine-grained locks "and the first wrong one is a machine that corrupts itself occasionally." Cost: two CPUs cannot be in the kernel at once; benefit: ring 3 (where programs spend their time) needs no lock, so parallelism is kept where it matters. Made tractable by the kernel being non-preemptive: syscalls arrive through an interrupt gate (IF cleared), so no timer lands mid-syscall, and "there is no path that takes this lock twice."

- **Kernel stacks 32 KiB** (`TASK_STACK_SIZE 32768`, sched.h:32). Sized against the deepest path: certificate verification holding a 4096-bit modulus through modular exponentiation, ~16 KiB, which used to leave only 168 bytes of margin at the old 16384. Every stack is *painted* with `0xC5C5C5C5C5C5C5C5` (sched.c:38) so the bottom word is a guard and the leftover paint is a high-water mark.

- **Per-CPU `current`** (`current_of[SMP_MAX_CPUS]`, sched.c:85). Was one global `current`; SMP forced "what is running" to become a per-processor question via `smp_this_cpu()`.

- **Idle tasks, one per CPU** (`idle_of[SMP_MAX_CPUS]`, sched.c:144). Before them, `pick_next` returned the task that had just asked to sleep, so "a sleep returned with no time passed whenever nothing else wanted the processor." The idle task halts (cools the CPU) and charges its ticks to itself as idle.

- **Copy-on-write fork** (paging.c). Was eager copy; a shell's `a | b` is two forks+execs whose copies are immediately discarded by exec, so COW shares frames and copies on first write.

- **Demand paging everywhere**: the heap (sbrk), mmap, and even the user stack are all "a promise" -- pages arrive on fault. "the cost of an address space is what it touches rather than what it asked for."

- **Two-layer FD model** (fd.h): open file descriptions (machine-wide, refcounted, hold position) vs per-process fd arrays (index into them). This is what makes stdin/stdout/stderr, redirection (dup2), and fork inheritance fall out for free.

- **Signals are minimal**: 3 signals (INT/KILL/TERM), default action = die, plus ignore and handlers. Default action taken in the scheduler (the one place a spinning program passes through); handler delivery happens on the way back to ring 3 (only there is the task's stack mapped).

- **The numbers are the interface**: syscall numbers are frozen; a retired call leaves a gap rather than reusing the number (44 = old SYS_GETARG).

---

## 3. File-by-file detail

### include/sched.h

**`vma_t`** (sched.h:10): one requested mapping -- `u64 base` (page-aligned, 0 = free slot), `u64 len` (whole pages), `u32 prot` (PROT_READ/PROT_WRITE). `VMA_MAX 16`.

**`task_state_t`** (sched.h:38): `TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED, TASK_DEAD`. Comment: BLOCKED differs from SLEEPING -- a sleeper has a wake time, a blocked task waits for an event and may have no deadline; the scheduler skips both but only BLOCKED can be woken early.

**`task_t`** (sched.h:55-162), field by field:
- `u64 rsp` -- saved kernel stack pointer (points at the saved `registers_t` frame).
- `u64 stack_base` -- bottom of the 32 KiB kernel stack.
- `u8 fpu[FPU_AREA + 16]` -- FXSAVE area, FPU_AREA=512, +16 slack so the used address can be 16-byte aligned (heap only promises 8). `fpu_area_of()` in sched.c returns the aligned pointer.
- `u32 pid`; `char name[32]`.
- `task_state_t state`.
- `u64 wake_at` -- tick to wake on; 0 = no deadline.
- `const void *wait_on` -- channel it is blocked on, or null.
- `int exit_status` -- set when dead.
- `bool reaped` -- status collected.
- `u64 died_at` -- finish tick, for the reap grace period.
- `u32 slices` -- times scheduled.
- `u64 idle_ticks` -- of those slices, ones spent halted (for the monitor; "slices minus these is what the task actually did").
- `u64 dir` -- page directory phys addr; 0 = kernel's.
- `u64 brk, brk_base` -- heap break, 0 until first sbrk.
- `u32 parent_pid` -- who forked it; 0 for kernel-started.
- `bool user` -- runs in ring 3.
- `char cwd[TASK_CWD_MAX]` (128) -- working dir, inherited at creation.
- `i16 fd[TASK_MAX_FD]` (16) -- per-task descriptor array; each entry indexes the open-file table or is -1. Copied by fork, kept by exec.
- `u32 sig_pending` -- bit per pending signal, acted on by the scheduler.
- `u32 sig_running` -- bit per signal currently inside its handler (blocks re-entry).
- `u64 sig_handler[SIG_MAX]` (32) -- SIG_DFL/SIG_IGN/ring-3 address per signal. Inherited across fork, cleared by exec.
- `vma_t vma[VMA_MAX]`; `int nvma` -- mmap regions.
- `u64 sig_trampoline` -- ring-3 return address for handlers (program supplies it; kernel has no ring-3 code).
- `int on_cpu` -- which CPU runs it, or -1. Distinct from `state`: state = wants to run, on_cpu = already running.
- `struct task *next` -- circular list link.

Constants: `VMA_MAX 16`, `TASK_CWD_MAX 128`, `TASK_STACK_SIZE 32768`, `TASK_MAX_FD 16` (asserted == FD_MAX in fd.c:24). `REAP_GRACE` = 10s (defined in sched.c:71 as `10u*100u` ticks).

### kernel/sched.c

**Kernel lock** (sched.c:97-118): a `spinlock_t kernel_lock` + `volatile int lock_holder` (-1 when free). `kernel_lock_acquire` spins then records holder; `kernel_lock_try` non-blocking; `kernel_lock_release` clears holder then unlocks; `kernel_lock_held_here` compares `lock_holder` to `smp_this_cpu()`.

**Stack painting**: `STACK_PAINT 0xC5C5...` (sched.c:38). `sched_paint_stack(base)` and static `paint_stack(u8*)` fill STACK_SIZE/8 words. `task_stack_headroom(t)` (sched.c:61) counts leading paint words × 8 = untouched bytes (high-water mark).

**Idle** (sched.c:146): `idle_entry` loops `hlt()` and adds elapsed ticks to `me->idle_ticks`. `is_idle(t)` / `task_is_idle(t)` scan `idle_of[]`.

**`task_create(name, entry)`** (sched.c:177): kcalloc task + kmalloc 32 KiB stack, paint, assign pid (`next_pid++`, starts at 1), TASK_READY, on_cpu=-1, `fpu_blank`, `inherit_cwd`, `fd_table_init` (3 consoles, not inherited -- "Started rather than forked"). Builds a `registers_t` frame at `top - sizeof(registers_t)` with cs=GDT_KERNEL_CODE, ss=GDT_KERNEL_DATA, rflags=0x202 (IF set), int_no=32; appends to circular list.

**`task_fork(name, dir, frame, child_rax)`** (sched.c:233): the *task-record* half of fork (address space cloned by caller `sys_fork`). Copies from parent: `cwd`, `sig_handler[]`, `sig_trampoline`, `brk`/`brk_base`, `vma[]`/`nvma`, whole `fpu[]`; sets `sig_pending=0`, `sig_running=0`, `parent_pid`. Clones fd table (`fd_table_clone`, bumps refs). Copies the interrupt `frame` verbatim, then overwrites `f->rax = child_rax` (0 for the child). "both sides come back from the same call with different answers."

**`task_create_user(name, dir, entry, stack_top)`** (sched.c:310): like task_create but frame has cs=USER_CODE_SEL, ss=USER_DATA_SEL (RPL 3), rsp=stack_top, dir set, user=true.

**Lookups**: `task_current`=`cur()`; `task_list`=head; `task_by_pid` walks ring; `task_alive(pid)` = found && state≠DEAD.

**`task_wait(pid)`** (sched.c:378): if no such task return -1; loop `while state != TASK_DEAD` calling `wait_on(t, 1000)` (waits on the task record's own address). On timeout, re-checks the task still exists (may have been reaped). Returns `exit_status` and sets `reaped=true` (which lets the reaper free it).

**`pick_next(from)`** (sched.c:428): one lap of the ring starting after `from`. Reasons captured in comments:
- If `me!=0 && smp_work_pending(me) && idle_of[me]`, returns own idle task (a CPU with a half-frame handoff pending must not take a program).
- Promotes SLEEPING→READY when `now >= wake_at`; promotes BLOCKED→READY when it has a deadline that passed (clears `wake_at` but *leaves* `wait_on` set, so the waiter can tell a timeout from a real wake).
- For READY/RUNNING: skips idle tasks, skips tasks whose `on_cpu` is another CPU, and **on APs (`me!=0`) skips non-user tasks** (kernel tasks stay on the boot CPU -- "kernel code on two processors with nothing between them" otherwise).
- Fallback: own idle task, else `from`.

**`scheduler_switch(rsp)`** (sched.c:479) -- called from isr_dispatch on int 32 (timer), VEC_YIELD, VEC_LOCAL_TIMER:
1. If not started or no head, return rsp unchanged.
2. `signal_take_pending()` -- default-action signals acted on before choosing.
3. Save `cur()->rsp = rsp`, `fpu_save`. **Stack-guard check**: if `*stack_base != STACK_PAINT`, `panic("the %s task ran off the end of its kernel stack")`.
4. RUNNING→READY; `on_cpu = -1` (release before choosing so another CPU can pick it).
5. `next = pick_next(cur())`; set_cur; on_cpu=me; RUNNING; `slices++`; if user, `sched_note_user_slice`.
6. `fpu_restore`; `tss_set_stack(stack_base + STACK_SIZE)` (where the next ring-3 interrupt lands).
7. Switch CR3 to `next->dir` (or kernel dir) if different.
8. **Reaper**: walk the ring (bounded to 4096), free any TASK_DEAD task that is `reaped || (died_at && now > died_at + REAP_GRACE)`, never `cur()` or `head`; frees its directory (`paging_free_directory`) then stack then record. Address space switch happens *before* this so freeing a CR3 in use is avoided.
9. Return `cur()->rsp`.

**`sched_adopt_ap(cpu, stack_base)`** (sched.c:560): builds a task record for an AP adopting the stack it booted on, state=RUNNING, on_cpu=cpu, name "idle", inserted behind head under the lock; sets `idle_of[cpu]` and `current_of[cpu]`.

**`sched_start()`** (sched.c:587): creates `idle_of[0]` last (so boot count excludes it), `kernel_lock_acquire()`, `started=true`, `sti()`, `for(;;) hlt()` -- the first timer tick enters a task.

**`task_yield()`** (sched.c:607): `int VEC_YIELD` (0xF1) -- its own vector, NOT the timer's, so it does not run the timer handler (which would count a phantom tick -- see idt.h:31 long comment).

**`task_sleep(ms)`** (sched.c:656): converts ms→ticks rounding up `((ms*hz+999)/1000)`, floors at 1 tick for any ms>0 (fixes short sleeps truncating to zero), sets wake_at + SLEEPING, yields. Before scheduler exists, falls back to `sleep_ms`.

**`task_idle_wait()`** (sched.c:635): if interrupts disabled, returns (a halt with IF clear never ends); else `hlt()` and add elapsed to idle_ticks. Whole ticks only, biased toward reporting busy.

**`task_exit_with(status)`** (sched.c:668): releases everything the task holds -- `winsrv_release`, `fd_table_release`, `syscall_release` -- sets exit_status, died_at, DEAD, `wake_all(cur())` (wake waiters on the record), then `for(;;) task_yield()`. `task_exit()` = exit_with(0).

**`task_state_name(s)`** (sched.c:620): a switch (not a table) -- a 4-entry table against 5 states once made `ps` print machine bytes for blocked/dead.

### kernel/syscall.c

**`user_range_ok(addr, len)`** (syscall.c:56): the pointer validator. len==0 → true; addr must be ≥ `USER_MIN` (=USER_SPACE_BASE = 0x8000000000); reject wrap (`addr+len < addr`); then **every page** in the range must satisfy `virt_is_user_in(current_dir, page)` -- i.e. present AND user-accessible at every paging level. Comment explains why "mapped" is insufficient: every address space inherits the kernel's high mappings (framebuffer aperture, device register windows), which are present but lack the user bit.

**`caller_pid()`** (syscall.c:70): current task's pid or 0.

**`copy_path(addr, out, cap)`** (syscall.c:77): copies a NUL-terminated string in byte-by-byte, checking `user_range_ok` on each byte; fails if no terminator within cap. Copied in "before anything looks at it" (TOCTOU: another thread could change it).

**argv machinery** (syscall.c:126-165): `ARGV_BYTES 2048`. `argv_t { char *store; const char *word[USER_ARGV_MAX]; int count; }`. `argv_take(a, vec, count)`: no vector = no words (not a failure); rejects count > USER_ARGV_MAX (64); validates the pointer array; kmallocs a 2048-byte store and copies each string in via copy_path. `argv_drop` frees.

Handlers (each `static i64 sys_*(registers_t *r)`), arguments come in rbx/rcx/rdx (see §5 ABI):
- `sys_exit` -- `task_exit_with((i32)r->rbx)`.
- `sys_spawn` -- copy path, `vfs_slurp`, `user_spawn_elf(path,...)`, free image. Names the task after the path (taskbar matching).
- `sys_spawn_argv` -- copy path, `argv_take` from rcx/rdx, slurp, `user_spawn_elf_argv` (with argv, or with path as sole arg if count==0).
- `sys_fork` (syscall.c:198): requires parent->dir; `paging_clone_directory(parent->dir)`; `cli()` across `task_fork(parent->name, dir, r, 0)` then restore IF (child runnable the instant it's on the list -- must be named first); returns child pid.
- `sys_exec` (syscall.c:228): copy path + argv *before* teardown (they live in the space about to be replaced); slurp; `paging_new_directory`; `elf_load`; `user_build_stack`; then `cli()`, set t->dir, `paging_switch`, reset brk=0/brk_base=0, `signal_forget_handlers`, `user_drop_mappings`, rename task to basename, **rewrite `*r`** to enter the new program (rip=entry, cs/ss=USER, rsp, rflags=0x202, int_no=32); restore IF; `paging_free_directory(old)`. Never returns on success.
- `sys_getppid` → parent_pid.
- `sys_sbrk` → `user_sbrk((i64)r->rbx)`.
- `sys_wait` → `task_wait((u32)r->rbx)`.
- `sys_kill` (syscall.c:321): refuses killing self; requires target exists & not DEAD; calls `signal_end_task(pid, -1)`. Comment: "any task other than the one asking" -- no parent check yet.
- `sys_signal` → `signal_disposition(pid, rbx, rcx how, rdx trampoline)`.
- `sys_poll` (syscall.c:355): rejects n > POLL_MAX (16); validates buffer; copies pollfds into a kernel `set[POLL_MAX]`; `fd_poll`; re-validates buffer (it sleeps, could vanish); copies revents back. **Note: this is the SYS_POLL handler even though the surrounding comment block at 349-354 describes sigreturn -- a stale/misplaced comment (see §10).**
- `sys_rename` → two paths, `vfs_rename`.
- `sys_fsync` → `fd_sync`.
- `sys_mmap` → `user_mmap(r->rbx len, (int)r->rcx prot)`.
- `sys_munmap` → `user_munmap(rbx, rcx)`.
- `sys_sigreturn` (syscall.c:398): if `signal_return(r)` returns true, return `r->rax` (the restored rax); otherwise `task_exit_with(139)` (a forged/invalid frame -- do not resume).
- `sys_sigsend` → `signal_send(rbx pid, rcx sig)`.
- `sys_win_resizable`, `sys_win_resize`, `sys_clip_set`/`get`, `sys_sound_info`/`write`, `sys_power`, `sys_tasks`, `sys_putc`, `sys_write`, `sys_getpid`, `sys_ticks`, `sys_sleep`, `sys_read_file`, `sys_open`/`close`/`fread`/`fwrite`/`dup`/`dup2`/`pipe`/`seek`/`unlink`/`mkdir`/`rmdir`/`readdir`/`stat`/`chdir`/`getcwd`, sockets, window server, `sys_sysinfo`.
- `sys_write` (syscall.c:519): buf in rcx, len in rdx, caps len at 65536, validates, `fd_write(FD_STDOUT, ...)` -- every program's output goes through descriptor 1, so all became redirectable when this line changed.
- `sys_tasks` (syscall.c:477): index in rbx, out ptr in rcx; validates `sizeof(zelr_task_t)`; walks ring to the index'th task; fills zelr_task_t {pid,state,slices,idle=idle_ticks,user,name} and memcpys; returns 1 (found) or 0.
- `sys_readdir` guarded by `_Static_assert(sizeof(zelr_stat_t.name) >= VFS_NAME_MAX)` (syscall.c:650) -- kernel writes VFS_NAME_MAX (64) bytes; name field is 64.

**Socket table** (syscall.c:712-914): `SOCK_MAX = TCP_MAX = 6`. `sock_t {bool open; bool secure; u32 owner; int tcp;}` × 6. `sock_of(raw)` checks bounds AND `owner == caller_pid()` (a program can't drive another's socket). `sys_connect`/`sys_connect_tls` (only one TLS session machine-wide, enforced by scanning for an existing secure socket), `sys_send`/`recv`/`disconnect`. `syscall_release(pid)` drops all sockets owned by a dying pid.

**Dispatch table** `TABLE[]` (syscall.c:1021-1085), designated-initializer array indexed by number. `N_SYSCALLS = sizeof(TABLE)/sizeof(TABLE[0])`. `syscall_handler(r)` (syscall.c:1089): `served++`; reads `n = r->rax`; if `n >= N_SYSCALLS || !TABLE[n]` → `r->rax = -1`; else `r->rax = TABLE[n](r)`. `syscall_init` registers it on vector 0x80. `served`/`syscall_count()`.

`N_SYSCALLS` = size of the array = highest index + 1. Highest number in the table is 63 (SYS_POLL), so `N_SYSCALLS == 64`. Retired 44 and the never-populated slots 56/57 are NULL entries → return -1 (see §10 for the 56/57 subtlety -- they ARE populated).

### kernel/user.c

Constants: `USER_CODE_BASE = USER_SPACE_BASE + 0x40000000` (=0x8040000000, where zelr.ld links programs). `USER_STACK_PAGES 16` (64 KiB initial stack). From user.h: `USER_HEAP_BASE = base+0x10000000`, `USER_HEAP_MAX = base+0x38000000`, `USER_MMAP_BASE = base+0x38000000`, `USER_MMAP_MAX = base+0x40000000`, `USER_STACK_TOP = base+0x50000000`, `USER_STACK_MAX = 1 MiB`.

**`alloc_user_page(dir, virt)`** (user.c:56): pmm_alloc_frame, zero it (frames identity-mapped so reachable by phys addr), `map_page_in(dir, virt, frame, PRESENT|RW|USER)`.

**`user_build_stack(dir, argc, argv, rsp_out)`** (user.c:90): maps USER_STACK_PAGES pages ending at USER_STACK_TOP; builds the System V initial stack in the top page from the top down: strings (argv[0] highest), then a 16-aligned block of `argc, argv[0..argc-1], NULL (argv end), NULL (empty environ)`. Returns rsp = USER_STACK_TOP - (PAGE_SIZE - at). Fails (maps nothing usable) if words don't fit or argc>USER_ARGV_MAX.

**`user_sbrk(delta)`** (user.c:156): lazily sets brk_base=brk=USER_HEAP_BASE on first call. delta==0 returns current. Negative: just lowers brk (never unmaps -- "the saving is a page"). Positive: reject wrap, reject > USER_HEAP_MAX; maps each new page with alloc_user_page; returns the old break. Returns 0 on failure (out of frames / over the ceiling).

**mmap** (user.c:230): `user_mmap(len, prot)` rounds len up to pages, rejects > MMAP window size, finds a free vma slot (reuses base==0 slots, else grows nvma up to VMA_MAX), then walks USER_MMAP_BASE upward a page at a time to the lowest gap that fits (via `vma_clear`), records base/len/prot, returns base. Nothing is mapped. `user_munmap(at, len)` -- whole ranges only (matches base && len exactly); frees the pages that arrived (via virt_to_phys_in), clears the vma, `paging_switch` to flush.

**`user_fault_fill(addr, err)`** (user.c:286) -- called from the page-fault path in isr_dispatch:
- `if (err & 1) return false` -- present page = not a missing page (COW already had its chance).
- **Stack growth**: if `addr < USER_STACK_TOP && addr >= USER_STACK_TOP - USER_STACK_MAX` (1 MiB window), map a fresh zeroed page. Comment: the stack is a region every program has and none declared, so a wild pointer inside the 1 MiB window silently gets a page.
- Otherwise find a `vma_holding(t, addr)`; if none → false (real bad pointer, program ends).
- If write (`err&2`) to a read-only vma → false.
- Else map a fresh zeroed page with RW iff PROT_WRITE. Double-fault race handled: if already mapped (another CPU won) return true.

**`user_drop_mappings()`** -- clears all vma, nvma=0 (exec).

**Spawn entry points**:
- `user_spawn_stub(name)` (user.c:351): loads the built-in `user_stub_start..end` flat blob (userstub.S -- prints "ring3\n" via SYS_PUTC then SYS_EXIT) at USER_CODE_BASE.
- `user_spawn_flat(name, image, size)`.
- `user_spawn_elf(name, image, size)` → `user_spawn_elf_argv(..., 1, &name)`.
- `user_spawn_elf_argv(name, image, size, argc, argv)` (user.c:391): new dir, `elf_load`, `user_build_stack`, `task_create_user`. Returns pid or a negative ELF_ERR_*/ELF_ERR_MEMORY.

### kernel/elf.c -- the loader is ELF64 (not ELF32)

`elf.c:1` header comment says "ELF64 loader." Structs `elf64_hdr_t` and `elf64_phdr_t` are 64-bit. Constants: ET_EXEC=2, EM_X86_64=62, PT_LOAD=1. `USER_LOAD_MIN = USER_SPACE_BASE`, `USER_LOAD_MAX = USER_SPACE_BASE + 0x4FF00000`.

**`elf_load(dir, image, size, entry_out)`** validation order (elf.c:59):
1. size < sizeof(hdr) → ELF_ERR_SHORT.
2. magic 0x7F 'E' 'L' 'F' → ELF_ERR_MAGIC.
3. `ident[4]!=2 || ident[5]!=1` (must be 64-bit little-endian) → ELF_ERR_CLASS.
4. `type != ET_EXEC || machine != EM_X86_64` → ELF_ERR_TYPE.
5. phoff==0 or phentsize < sizeof(phdr) → ELF_ERR_SHORT.
6. phoff + phnum*phentsize > size → ELF_ERR_OVERFLOW.
7. entry outside [USER_LOAD_MIN, USER_LOAD_MAX) → ELF_ERR_RANGE.
8. Per PT_LOAD segment (memsz>0): filesz>memsz → OVERFLOW; offset+filesz overflow/past file → OVERFLOW; vaddr < MIN, vaddr+memsz wrap, vaddr+memsz > MAX → RANGE.
9. Maps the covered page range (RW|USER, zeroed for .bss), copying filesz bytes at the true offset (handles segments sharing a page).

`elf_error(code)` maps codes to strings. Note segments are always mapped RW|USER regardless of PF_W (no W^X enforcement; the loader ignores p->flags).

### kernel/fd.c

`OF_MAX 32` open file descriptions. `ofkind_t {OF_FREE, OF_CONSOLE, OF_FILE, OF_PIPE}`. `ofile_t` (fd.c:34): kind, `u16 refs`, `char path[VFS_PATH_MAX]`, `u8 *data`, `u32 size/cap/pos`, `bool writable/dirty`, `pipe_t *pipe`, `bool writing`. Slot 0 (`OF_CONSOLE_SLOT`) is the console, refs=1, never freed.

`_Static_assert(TASK_MAX_FD == FD_MAX)` (fd.c:24). Kernel's own fd table `kernel_slots[FD_MAX]` used when `task_current()` is null (self-test, early boot). `table()` returns current task's fd[] or the kernel table.

- `fd_table_init(slots)`: 0/1/2 → console slot, 3..15 → -1.
- `fd_table_clone(dst, src)`: copies indices, bumps refs on non-console descriptions (fork).
- `fd_table_release(slots)`: unrefs all, sets -1 (exit).
- `of_alloc(kind)`: first free slot 1..31, refs=1.
- `of_unref(oi)`: console/invalid → true; decrement refs; on last ref, for OF_FILE flush if dirty (`vfs_write`) + kfree data, for OF_PIPE `pipe_close(pipe, writing)`; zeroes slot.
- `fd_open(path, flags)` (fd.c:290): resolve to abs; reject directories; reject missing without O_CREATE; **reject O_WRITE on a `vfs_generated` (/sys) path**; allocate fd number first, then description; read existing content unless O_TRUNC; pos = size if O_APPEND else 0.
- `fd_read`/`fd_write`: dispatch on kind. Console read = line-buffered `console_read`; pipe checks the correct end; file copies to/from `data` growing via `grow_to` (doubling, min 256). Writing wrong pipe end / non-writable file → -1.
- `fd_seek(fd, offset, whence)`: files only (pipe/console → -1); whence 0=set,1=cur,2=end; negative result → -1.
- `fd_size`: pipe → pipe_count; file → size.
- `fd_close(fd)`: clears the slot to -1 and `of_unref`.
- `fd_sync(fd)` (fd.c:480): file → vfs_write if dirty then clear dirty; non-file → true; if not dirty still `diskfs_flush`.
- `console_read` (fd.c:214): line editing here (not per program). Calls `signal_console_reader(me->pid)` each read (records who ctrl-C targets). Handles `\n` (echo + return), `\b`, ctrl-D (4 = end of input, returns n), **ctrl-C (3)**: prints `^C\n`, returns 1 with `out[0]='\n'` (an empty line, NOT EOF -- so ctrl-C doesn't close the shell). `console_pause` opens IF for the halt then closes it (a syscall arrives with IF clear; waiting for a key with the door shut would hang).
- `fd_ready_now(fd, want)` (fd.c:419): console = POLLIN if key waiting, always POLLOUT; file = POLLIN|POLLOUT; pipe write end = POLLERR if no readers else POLLOUT if room; pipe read end = POLLIN if bytes, else POLLIN|POLLHUP if no writers. Always adds POLLERR|POLLHUP|POLLNVAL to the mask.
- `fd_poll(fds, n, timeout_ms)` (fd.c:447): **polls by re-looking, not by waking** -- the kernel's wait has no way to wait on multiple queues. Loops: check all, return count if any ready; if timeout 0 return 0; if deadline passed return 0; else `task_sleep(one tick)`. Up to one tick (10 ms) latency, stated honestly.
- `fd_dup(fd)`: lowest free slot pointing at same description, refs++.
- `fd_dup2(fd, to)`: unref old `to`, point at `fd`'s description, refs++. `fd==to` returns to.
- `fd_pipe(ends)` (fd.c:521): reserve two fd numbers (holding r at console slot so the second call skips it), `pipe_new`, two OF_PIPE descriptions (reader writing=false, writer writing=true). Rolls back cleanly on any failure.
- `fd_live()`: count of non-free descriptions (leak check).

### kernel/pipe.c

`PIPE_SIZE 4096` (fixed, small -- "a pipe is not storage"). `PIPE_GIVE_UP_MS 10000` (deadlock escape), `PIPE_POLL_MS 200` (safety-net re-check under the wakeups). `pipe_t` (pipe.h:25): `u8 buf[4096]`, `u32 head/tail/count`, `u16 readers/writers`, `u8 has_data, has_space` (distinct wait-channel addresses).

- `pipe_new()`: kcalloc, readers=1, writers=1, `live++`.
- `pipe_read(p, dst, len)`: loop while count==0 -- if writers==0 return 0 (EOF); else `wait_on(&has_data, 200)`, giving up after 10 s (returns 0). Copies min(count,len), advances head, `count -= n`, `wake_all(&has_space)`. Never returns less than available.
- `pipe_write(p, src, len)`: loop until all written -- if readers==0 return `done?done:-1` (no reader = write into nothing); if full `wait_on(&has_space, 200)`, give up after 10 s; else copy a chunk, `wake_all(&has_data)` after each chunk (so a reader on a large write wakes early). Blocks until it all fits.
- `pipe_close(p, write_end)`: decrement the relevant counter; last writer → `wake_all(&has_data)` (readers see EOF); last reader → `wake_all(&has_space)`; both zero → `live--`, kfree.
- `pipe_count`, `pipe_live`.

### kernel/signal.c

Signals: `SIGINT 2`, `SIGKILL 9`, `SIGTERM 15`, `SIG_MAX 32`. `SIG_DFL 0`, `SIG_IGN 1`; anything above = a ring-3 handler address.

- `console_pid` (static): task that last read the console. `signal_console_reader(pid)` sets it.
- `signal_end_task(pid, status)` (signal.c:29): the single "end a task that isn't running" path -- `winsrv_release`, `fd_table_release`, `syscall_release`, set exit_status/died_at/DEAD, `wake_all(t)`. One copy so no ending path forgets a resource.
- `signal_send(pid, sig)`: validate 1..31; set `sig_pending |= 1<<sig`.
- `signal_disposition(pid, sig, how, trampoline)`: reject SIGKILL; if handler (how not DFL/IGN), require trampoline and both how & trampoline ≥ USER_SPACE_BASE (no ring-0 jump), store trampoline; set `sig_handler[sig]=how`.
- `signal_forget_handlers(pid)`: set every handler>SIG_IGN back to DFL, clear trampoline and sig_running (exec).
- `signal_take_pending()` (signal.c:93) -- scheduler-side: over every non-dead task with sig_pending, act on the ones that are SIG_DFL (default = die), drop SIG_IGN ones, **leave handler signals pending** (can't build a handler frame here -- the task's memory isn't mapped). For the lowest live default signal, `signal_end_task(pid, 128+sig)`.
- `stack_is_there(lo, hi)`: every page user-accessible in current dir.
- `signal_deliver(r)` (signal.c:169) -- on the way back to ring 3 (isr_dispatch:285, only for `from_user(back)`): find the lowest pending signal with a handler; skip if already in sig_running; compute `sp = (r->rsp - sizeof(registers_t)) & ~15`; if the stack isn't there, drop the signal and `signal_end_task(128+sig)`; else save `*saved = *r` on the user stack, push the trampoline address at `sp-8`, clear pending bit, set sig_running bit, and rewrite the frame: rsp=back, rip=handler, rdi=sig, rax=0. One per return (handler runs interruptibly).
- `signal_return(r)` (signal.c:222): `at = r->rsp`; reject unaligned; require stack present; read `saved`; **validate**: `(cs&3)==3 && (ss&3)==3 && rip>=USER_SPACE_BASE`; restore `*r=*saved` keeping int_no/err_code; force cs/ss = USER selectors; `rflags = (saved->rflags & 0x0CD5) | 0x202` (mask to allowed flags, force IF on); clear the matching sig_running bit. Returns false on any bad frame (caller then kills with 139).
- `signal_interrupt()` (signal.c:262) -- ctrl-C: if no console_pid return 0; send SIGINT to every non-dead task whose `parent_pid == console_pid` (the reader's running children); if any, return count; else send SIGINT to console_pid itself. So ctrl-C at a bare shell prompt does nothing (shell has SIGINT ignored, no children).

Raised from `keyboard.c:46` (`interrupting()` on byte 3) and `serial.c:56`.

### kernel/wait.c

Counters `wakeups`, `blocked`. `wait_on(channel, timeout_ms)` (wait.c:30): if no task/channel, spin via sleep_ms and return false. Else `cli()`, set `wait_on=channel`, `wake_at = timeout? now + timeout*hz/1000 + 1 : 0`, state=BLOCKED, blocked++, restore IF, `task_yield()`. On return, `woken = (wait_on==0)` -- cleared by a real wake; a deadline expiry in pick_next leaves wait_on set, so a timeout returns false. `wake(channel, only_one)`: `cli()`, walk ring, for each BLOCKED task with matching wait_on clear wait_on/wake_at, set READY, wakeups++, blocked--, break if only_one. `wake_all`/`wake_one`. Everything runs with IF off (the block/actually-block window is where a missed wakeup would hide).

### kernel/apps.c -- the "about" window

The one window the kernel still draws itself, because a ring-3 program "has no way to ask these questions." `about_render(w)` (apps.c:23) draws: version (`KERNEL_VERSION`), "an operating system from scratch", memory used/total (`pmm_*_frames()*4`), heap used/total, display WxH, uptime seconds, `task_count()`, `fs_count()`, and two hint lines. `about_on_mouse` re-renders on any press (refresh figures). `app_about()` creates or raises a 322×300 window at (682,120). `app_about_forget(w)` clears the static handle on close.

### tools/abicheck.py

Compares the two ABI sides. `KERNEL = include/syscall.h`, `USER = sdk/zelr.h`. `PAIRS` = the 5 shared structs: `(zelr_task_t, zelr_task)`, `(zelr_stat_t, zelr_stat)`, `(zelr_sysinfo_t, zelr_sysinfo)`, `(zelr_netinfo_t, zelr_netinfo)`, `(sound_info_t, zelr_sound)`. `SAME` maps kernel widths to sdk names (`unsigned int→u32`, `unsigned long long→u64`, `int→i32`). Strips comments, parses `typedef struct {...} name;`, splits fields (handles `u32 a, b;` and arrays). `check_numbers`: every `#define SYS_* n` in each file; flags names only-in-kernel, only-in-user, disagreeing values, and two calls sharing a number. Struct check compares field type + array length in order (not field names). `check_sse.py` etc. are separate.

### tools/ring3check.py

Boots QEMU once (256 MiB) via the harness and runs 19 ring-3 test ELFs by `exec /bin/<name>`, each with a PASS marker and timeout. The process/syscall-relevant suites: **forktest** (fork and exec), **fdtest** (descriptors), **sleeptest** (a sleep that sleeps), **cowtest** (what a fork copies), **argvtest** (what a program was started on), **sigtest** (being told about a signal), **faulttest** (a program that faults, machine survives), **maptest** (mmap costs what it's used), **polltest** (waiting on several descriptors). Also alloctest (over sbrk). Checks each marker appears; also checks jstest reports ≥86 cases.

---

## 4. Control flow and lifecycles

**Boot → scheduler** (main.c): `sched_init()` (before SMP, so AP-adopted idle tasks survive) → `smp_init()` (APs adopt stacks as idle tasks) → `syscall_init` + `winsrv_init` → `usb_start_service`/`net_start_service` (kernel tasks) → `task_create("selftest"|"init")` → `net_dhcp_start` → `sched_start()` (creates idle_of[0], takes kernel lock, sti, halts until first tick).

**Syscall path**: ring 3 `int 0x80` → gate (DPL 3, IF cleared) → isr128 → isr_common saves 15 regs → `isr_dispatch(r)`. Since arriving from ring 3, `kernel_lock_held_here()` is false and it's not a timer, so `kernel_lock_acquire()`. `handlers[0x80] = syscall_handler` runs → TABLE[rax](r) → result in r->rax. No scheduler switch on int 0x80 (only int 32/241/242). On the way out: `back = resume`; if `from_user(back)` → `signal_deliver(back)`; if `from_user(back) || task_is_idle(current)` → `kernel_lock_release()`. iretq.

**"Frame being returned through" rule** (idt.c:260-287): whether to release the lock is decided by the *saved frame being resumed*, not by whether the task is a user task -- because "a program preempted in the middle of a system call is a program by that test" and would resume without the lock while still running kernel code. The lock is released only when returning to ring 3 (`(cs&3)==3`) or when the CPU is about to run its idle task (a sleeping CPU holding the lock would freeze the machine).

**Preemption**: 8254 timer on the boot CPU (int 32) counts ticks and runs `on_tick` (usb/ps2/synaptics/sound/rng polling). Each CPU's LAPIC timer (VEC_LOCAL_TIMER 0xF2) drives its own switching. A CPU that can't grab the lock on its LAPIC tick skips (`smp_note_lock_miss`) rather than blocking. int 32, VEC_YIELD (0xF1), VEC_LOCAL_TIMER all call `scheduler_switch`.

**Task lifecycle**: create (READY) → RUNNING (scheduler picks, on_cpu set) → back to READY at slice end; `task_sleep`→SLEEPING (wake_at); `wait_on`→BLOCKED (wait_on channel, optional wake_at); wake_all/deadline→READY; `task_exit_with`/`signal_end_task`→DEAD (died_at set, resources released, waiters woken). DEAD record kept until `reaped` (task_wait collected status) or `died_at + REAP_GRACE` (10 s) passes; reaper in scheduler_switch frees dir+stack+record.

**fork** (2-return trick): `sys_fork` clones the directory (COW) → `task_fork` copies the interrupt frame with rax=0 for the child. Parent returns child pid, child returns 0 from the same `int 0x80`. First write to a shared page faults into `paging_resolve_cow` (idt.c:199: `(err&0x3)==0x3` = write to present) which copies if `pmm_holders > 1` else re-enables RW.

**exec**: build the new address space fully before tearing the old one down; rewrite the interrupt frame in place so iretq lands in the new program; free the old directory. Keeps fd table, resets brk/handlers/mappings.

**signal delivery state machine**: send sets `sig_pending`. Default signals: `signal_take_pending` (scheduler) → `signal_end_task(128+sig)`. Handler signals: `signal_deliver` (return-to-ring-3) builds a frame on the user stack, sets sig_running, enters handler with rdi=sig; handler runs in ring 3, ends with `ret` into the trampoline (`__zelr_sigreturn`, sdk/zelr.h:604) which does `int 0x80` SYS_SIGRETURN → `signal_return` restores the frame and clears sig_running. A same-signal re-entry is blocked by sig_running while inside.

---

## 5. Interfaces (the complete syscall table + ABI)

**int 0x80 register convention** (sdk/zelr.h:198): `syscall(n,a,b,c)` = `int $0x80` with n→rax, a→rbx, b→rcx, c→rdx, result←rax, clobbers memory. So handlers read `r->rax`(number), `r->rbx`(arg1), `r->rcx`(arg2), `r->rdx`(arg3), return via `r->rax`. Every arg is a 64-bit `zelr_word` (`long long`) so a pointer isn't truncated. `sys_win_surface` is the only handler returning a pointer, which is why the table returns `i64`.

Complete table (number, kernel handler, sdk wrapper, args → return):

| # | Kernel handler | sdk wrapper | Args (rbx, rcx, rdx) | Returns / semantics |
|---|---|---|---|---|
| 0 | sys_exit | exit/zelr_exit | rbx=status | no return; task_exit_with |
| 1 | sys_putc | putc/zelr_putc | rbx=char | writes 1 byte to fd 1 |
| 2 | sys_write | write | rcx=buf, rdx=len | len≤65536, validated; fd_write(1); bytes or -1 |
| 3 | sys_getpid | getpid | -- | pid |
| 4 | sys_ticks | ticks | -- | timer_ticks (i32) |
| 5 | sys_sleep | sleep_ms | rbx=ms | task_sleep; 0 |
| 6 | sys_read_file | read_file | rbx=name, rcx=buf, rdx=cap | vfs_read; bytes or -1 |
| 7 | sys_win_create | win_create | rbx=title, rcx=w, rdx=h | handle or -1 |
| 8 | sys_win_surface | win_surface | rbx=handle | user address (pointer) or 0 |
| 9 | sys_win_size | win_width/height | rbx=handle | cw<<16\|ch or -1 |
| 10 | sys_win_poll | win_poll | rbx=handle, rcx=event* | 1 event / 0 none |
| 11 | sys_win_commit | win_commit | rbx=handle | 0/-1 |
| 12 | sys_win_close | win_close | rbx=handle | 0/-1 |
| 13 | sys_open | open | rbx=path, rcx=flags | fd or -1 |
| 14 | sys_close | close | rbx=fd | 0/-1 |
| 15 | sys_fread | zelr_fread/fread | rbx=fd, rcx=buf, rdx=len | len≤1 MiB; bytes or -1 |
| 16 | sys_fwrite | zelr_fwrite/fwrite | rbx=fd, rcx=buf, rdx=len | len≤1 MiB; bytes or -1 |
| 17 | sys_seek | seek | rbx=fd, rcx=off, rdx=whence | new pos or -1 |
| 18 | sys_unlink | unlink | rbx=path | 0/-1 |
| 19 | sys_mkdir | mkdir | rbx=path | 0/-1 |
| 20 | sys_rmdir | rmdir | rbx=path | 0/-1 |
| 21 | sys_readdir | readdir | rbx=path, rcx=index, rdx=zelr_stat* | 1/0/-1 |
| 22 | sys_stat | stat | rbx=path, rcx=zelr_stat* | 0/-1 |
| 23 | sys_chdir | chdir | rbx=path | 0/-1 |
| 24 | sys_getcwd | getcwd | rbx=buf, rcx=cap | length or -1 |
| 25 | sys_connect | connect | rbx=host, rcx=port | socket or NET_ERR_* |
| 26 | sys_send | send | rbx=sock, rcx=buf, rdx=len | len or -1 |
| 27 | sys_recv | recv | rbx=sock, rcx=buf, rdx=len | bytes/0/-2(NET_EOF)/-1 |
| 28 | sys_disconnect | disconnect | rbx=sock | 0/-1 |
| 29 | sys_resolve | resolve | rbx=host, rcx=u32* | 0/-1 |
| 30 | sys_netinfo | netinfo | rbx=zelr_netinfo* | 0/-1 |
| 31 | sys_sysinfo | sysinfo | rbx=zelr_sysinfo* | 0/-1 |
| 32 | sys_spawn | spawn | rbx=path | pid or -1 |
| 33 | sys_wait | wait_for | rbx=pid | exit status or -1 |
| 34 | sys_kill | kill | rbx=pid | 0/-1 |
| 35 | sys_tasks | tasks | rbx=index, rcx=zelr_task* | 1/0 |
| 36 | sys_win_resizable | win_allow_resize | rbx=handle | 0/-1 |
| 37 | sys_win_resize | win_resize | rbx=handle, rcx=w, rdx=h | 0/-1 |
| 38 | sys_clip_set | clip_set | rbx=text, rcx=len | len or -1 (len≤CLIP_MAX 65536) |
| 39 | sys_clip_get | clip_get/clip_len | rbx=buf, rcx=cap | cap==0 → length; else copied bytes |
| 40 | sys_sound_info | sound_info | rbx=sound_info* | 0/-1 |
| 41 | sys_sound_write | sound_write | rbx=frames, rcx=count | frames written; count capped 4096 |
| 42 | sys_power | power_off/power_reboot | rbx=POWER_OFF/REBOOT | no return on success, -1 else |
| 43 | sys_spawn_argv | spawnv/spawn_arg | rbx=path, rcx=argv[], rdx=count | pid or -1 |
| **44** | -- RETIRED (SYS_GETARG) | -- | -- | gap; not in TABLE, not in sdk |
| 45 | sys_connect_tls | connect_tls | rbx=host, rcx=port | socket or NET_ERR_* |
| 46 | sys_tls_status | tls_status/tls_why/tls_what | rbx=buf, rcx=cap, rdx=TLS_WHY/WHAT | length |
| 47 | sys_sbrk | sbrk | rbx=delta | old break or 0 |
| 48 | sys_fork | fork | -- | child pid / 0 / -1 |
| 49 | sys_exec | execv/exec | rbx=path, rcx=argv[], rdx=count | -1 on failure only |
| 50 | sys_getppid | getppid | -- | parent pid |
| 51 | sys_dup | dup | rbx=fd | new fd or -1 |
| 52 | sys_dup2 | dup2 | rbx=fd, rcx=to | to or -1 |
| 53 | sys_pipe | pipe | rbx=int[2] | 0/-1 |
| 54 | sys_signal | signal | rbx=sig, rcx=how, rdx=trampoline | 0/-1 |
| 55 | sys_sigsend | send_signal | rbx=pid, rcx=sig | 0/-1 |
| 56 | sys_win_text | win_set_text | rbx=handle, rcx=text, rdx=len | len or -1 |
| 57 | sys_win_find | win_find_query | rbx=buf, rcx=cap | query length or -1 |
| 58 | sys_sigreturn | (via __zelr_sigreturn trampoline) | -- | restored rax; never a normal return |
| 59 | sys_mmap | map | rbx=len, rcx=prot | address or 0 |
| 60 | sys_munmap | unmap | rbx=at, rcx=len | 0/-1 |
| 61 | sys_fsync | fsync | rbx=fd | 0/-1 |
| 62 | sys_rename | zelr_rename/rename | rbx=from, rcx=to | 0/-1 |
| 63 | sys_poll | poll | rbx=pollfd[], rcx=n, rdx=timeout_ms | ready count / 0 / -1 (n≤POLL_MAX 16) |

**Count reconciliation**: 63 distinct populated numbers (0–43, 45–63). 44 retired. `N_SYSCALLS = 64` (highest index 63 + 1). sdk/zelr.h line 11 says "forty-seven system calls" and line 31 of README says "sixty-three system calls" -- see §10. abicheck.py confirms both headers define the same SYS_* names and values (they do -- I verified all 63 match between include/syscall.h and sdk/zelr.h, including the out-of-order block 56/57 placed after 55 in both files, and 58/45/46 defined before the 48–57 block in both).

**Pointer validation summary**: every handler that takes a user pointer calls `user_range_ok`/`copy_path` before dereferencing; strings are copied into kernel buffers (`copy_path`, VFS_PATH_MAX=128, or 128-byte host buffers for connect/resolve). sys_poll re-validates after sleeping. Output structs are memcpy'd into the validated user buffer.

**Consumers (grep)**: sdk/zelr.h wraps all; userland shells (sh.c, term.c) use fork/exec/dup2/pipe/wait/signal/kill/spawnv; kernel shell.c and wm.c call user_spawn_elf(_argv); selftest.c/ring3check tests exercise everything. `syscall_release`/`winsrv_release`/`fd_table_release`/`signal_end_task` are the teardown fan-out used by exit, kill and fault.

**Depends on**: paging (map_page_in, paging_clone_directory, paging_resolve_cow, virt_is_user_in, paging_new/free_directory, paging_switch), pmm (alloc/free/share/holders), idt/isr (frame + dispatch + fault entry), smp (smp_this_cpu, smp_work_pending), timer (ticks/hz), gdt (selectors, tss_set_stack), fpu (save/restore/blank), vfs/fat, winsrv, tcp/tls/net, clipboard, sound, power, fb.

---

## 6. Concurrency, locking, memory ownership, invariants

- **Kernel lock** serializes all in-kernel execution across CPUs. Held from syscall/interrupt entry (from ring 3) to return to ring 3, and for the whole time a CPU runs a kernel task. Released only when resuming a ring-3 frame or a CPU's idle task. Never taken twice (kernel is non-preemptive; gates clear IF).
- **wait.c and the drivers use `cli()`/`sti()`** for the block/wake critical sections; wakeups usually come from interrupt handlers. The net/tcp/tls stack does not use wait queues -- it busy-polls `net_poll()` against `timer_ticks()` deadlines (kernel_lock held throughout), which works because the stack is single-threaded and callers are the only thing pumping frames aside from the 10 ms `net_task`.
- **fork interrupts-off window** (sys_fork): child must be named before a tick could schedule it.
- **on_cpu vs state**: a task is claimed by exactly one CPU; `on_cpu` is cleared before pick_next chooses so another CPU can grab it. Kernel (non-user) tasks never migrate off CPU 0.
- **Memory ownership**: each user page belongs to one directory and is freed with it (paging_free_directory frees only entries not shared with the kernel). COW frames are refcounted in pmm's `extra[]` byte array (holders past the first; cap 255). A frame with holders>1 is copied on write; the last holder re-enables RW. sbrk pages are never reclaimed until the directory dies. mmap pages that arrived are freed by munmap.
- **FD ownership**: open file descriptions are refcounted (`refs`); fork bumps, close/exit lower; description freed (and file flushed to disk) at refs 0. The console slot is never freed.
- **Pipe ownership**: readers/writers counts; pipe freed when both hit 0.
- **Invariants**: TASK_MAX_FD == FD_MAX (asserted); zelr_stat_t.name width ≥ VFS_NAME_MAX (asserted); every kernel stack's bottom word stays STACK_PAINT (checked every switch → panic); a handler's saved frame must return to ring 3 (validated in signal_return); a user pointer must be user-reachable at every page (user_range_ok).

---

## 7. Limits and magic numbers

| Constant | Value | Where | Meaning |
|---|---|---|---|
| TASK_STACK_SIZE | 32768 | sched.h:32 | kernel stack per task |
| STACK_PAINT | 0xC5C5C5C5C5C5C5C5 | sched.c:38 | stack fill / guard |
| REAP_GRACE | 1000 ticks (10 s) | sched.c:71 | dead-task retention |
| VMA_MAX | 16 | sched.h:8 | mmap regions per task |
| TASK_CWD_MAX | 128 | sched.h:18 | cwd length (=VFS_PATH_MAX) |
| TASK_MAX_FD / FD_MAX | 16 | sched.h:52 / fd.h:41 | fds per task |
| FPU_AREA | 512 | fpu.h:32 | FXSAVE area (+16 slack in task_t) |
| next_pid start | 1 | sched.c:163 | pids start at 1 |
| SMP_MAX_CPUS | 16 | smp.h:30 | current_of/idle_of size |
| USER_SPACE_BASE | 0x8000000000 | paging.h:62 | 512 GiB, user half |
| USER_HEAP_BASE | +0x10000000 | user.h:38 | heap start |
| USER_HEAP_MAX | +0x38000000 | user.h:39 | heap ceiling |
| USER_MMAP_BASE | +0x38000000 | user.h:62 | mmap window start |
| USER_MMAP_MAX | +0x40000000 | user.h:63 | mmap window end (= image) |
| USER_CODE_BASE / link addr | +0x40000000 (0x8040000000) | user.c:14, zelr.ld:5 | program image |
| USER_STACK_TOP | +0x50000000 | user.h:109 | top of stack |
| USER_STACK_MAX | 1 MiB | user.h:104 | max stack growth |
| USER_STACK_PAGES | 16 (64 KiB) | user.c:49 | initial stack |
| USER_LOAD_MIN/MAX | base .. base+0x4FF00000 | elf.c:42-43 | allowed segment range |
| USER_ARGV_MAX | 64 | user.h:25 | max argv words |
| ARGV_BYTES | 2048 | syscall.c:126 | argv string store |
| OF_MAX | 32 | fd.c:30 | open file descriptions |
| PIPE_SIZE | 4096 | pipe.h:23 | pipe ring buffer |
| PIPE_GIVE_UP_MS | 10000 | pipe.c:28 | pipe deadlock escape |
| PIPE_POLL_MS | 200 | pipe.c:33 | pipe wakeup safety net |
| POLL_MAX | 16 | fd.h:91 | fds per poll |
| SIG_MAX | 32 | signal.h:52 | signal bitmap width |
| SIGINT/KILL/TERM | 2/9/15 | signal.h | the three signals |
| SOCK_MAX / TCP_MAX | 6 | syscall.c:712 | concurrent sockets |
| N_SYSCALLS | 64 | syscall.c:1087 | table size (highest# 63 +1) |
| sys_write cap | 65536 | syscall.c:522 | max write |
| fread/fwrite cap | 1 MiB | syscall.c:582,589 | max file I/O |
| sound_write cap | 4096 frames | syscall.c:456 | per-call frame cap |
| CLIP_MAX | 65536 | clipboard.h:16 | clipboard size |
| fault exit status | 139 | idt.c:142 | killed-by-fault |
| signal exit status | 128+sig | signal.c:124 | killed-by-signal |

---

## 8. Tests

**Kernel selftest** (kernel/selftest.c, `-append selftest`):
- `test_user_access` (211): user_range_ok's rule -- kernel page above user space mapped but not user-reachable; user page reachable, its kernel neighbour in the same table not; unmapped not.
- `test_elf` (595) via `make_elf` (574): rejects short/bad-magic/wrong-class (`buf[4]=1` "claims 32 bit")/wrong-machine/kernel-range/overflow; accepts a well-formed ELF64, entry preserved.
- `test_userspace` (634): user_spawn_stub runs, reaches exit, issues ≥7 syscalls, address space reclaimed.
- `test_open_files` (918): fd==3 first; write/seek/overwrite/size/close; dup shares position; dup2 to 9; pipes (both ends, EOF, wrong-end refusals, no seek); `pipe_live()==0` and `fd_live()` back to baseline.
- `test_smp` (1182): per-CPU TSS, idle CPU asleep (spins<100), 20000 locked increments exact, each CPU ran its share.
- `test_waiting` (1311): a waiter blocks, isn't scheduled while blocked (slices unchanged), wakes on wake_all, exit status collected; wait on nonexistent pid = -1.
- `test_idle_accounting` (1351): every state name distinct and lowercase; spinning shows slices with 0 idle; waiting shows slices mostly idle; the two are distinguishable.
- `test_stack` (1718): headroom high-water mark ≥ ¼ of stack.
- `test_wait_timeout` (2920): a deadline-only wait returns and reports timeout (2) not woken, after ~its time.

**tools/ring3check.py**: forktest, fdtest, sleeptest, cowtest, argvtest, sigtest, faulttest, maptest, polltest, alloctest (all detailed in §3/§4). **tools/abicheck.py**: numbers + 5 structs. **tools/smpcheck.py**: `bg /bin/spin` ×6 on `-smp 4`, checks boot CPU and ≥1 other ran programs, each AP has its own clock (>200 ticks), machine still runs a program to completion. **tools/shcheck.py**: /bin/sh redirection (dup2), pipelines (2 and 3 stages), ctrl-C interrupting /bin/spin while the shell survives, built-ins (cd/pwd). Gate wiring: pipeline/gate.sh:264 runs abicheck, :302 runs ring3check.

---

## 9. How to extend

**Adding a syscall**: (1) `#define SYS_NEW n` in BOTH include/syscall.h and sdk/zelr.h with the SAME number (abicheck.py enforces it); pick the next free number -- do NOT reuse 44. (2) Write `static i64 sys_new(registers_t *r)` in syscall.c reading rbx/rcx/rdx, validating every user pointer with `user_range_ok`/`copy_path`. (3) Add `[SYS_NEW] = sys_new` to `TABLE[]`. (4) Add an `inline` wrapper in sdk/zelr.h. (5) If it shares a struct, add it to abicheck.py PAIRS and mirror the struct exactly (widths + array lengths). The `_Static_assert` pattern (fd.c:24, syscall.c:650) is the tool for "two numbers must agree" invariants.

**Adding a task field to copy on fork**: update `task_fork` (sched.c:259 parent branch) -- the comment there enumerates exactly what fork copies vs what exec resets; a field that should survive fork but not exec also needs handling in sys_exec (syscall.c:264-285).

**Adding a signal**: bump nothing (SIG_MAX 32 already), just define the number < 32; but the default action machinery in `signal_take_pending` treats all defaults as "die" -- a non-fatal default would need real per-signal logic there.

**Pitfalls the comments warn about**:
- zelr_stat_t.name / zelr_task_t widths: the kernel memcpys `sizeof(kernel struct)` into the user buffer, so a narrower user field is overwritten past its end (bit them twice -- `ls /` empty, monitor reading 0% -- hence the assert and abicheck).
- Short sleeps truncating to 0 ticks (sleep_ms and task_sleep now round up, floor at 1).
- Yield must use VEC_YIELD, not the timer vector, or every yield fabricates a tick and every deadline shrinks (idt.h:31).
- Building a handler frame or writing to a task's stack only works while that task's address space is loaded -- hence signal_deliver runs in isr_dispatch after the switch, and signal_take_pending (walking not-current tasks) can only take default actions.
- user_range_ok must check the user bit at every level, not just presence.
- Freeing the active CR3: scheduler_switch switches address spaces before reaping.
- A partial munmap/sbrk-down is deliberately not supported (would need to split a vma).

---

## 10. Doc drift and suspicious code (verified)

1. **Syscall count contradictions (README/header drift, confirmed).**
   - `sdk/zelr.h:11`: "The entire user-facing interface: **forty-seven** system calls." Actual populated wrappers/numbers = 63 (0–43, 45–63). 47 is stale (it was true around the SYS_SBRK era; SBRK is #47).
   - `sdk/README.md:31`: "zelr.h is the whole interface: **sixty-three** system calls." This one is correct (63 live numbers).
   - The project brief notes README variously says 57/58/63; the code has 63 live calls, 64 table slots, one retired number (44). No functional bug -- just comment drift.

2. **Misplaced comment above `sys_poll` (confirmed, cosmetic).** syscall.c:349-354 is a doc comment describing SYS_SIGRETURN ("Asked for by a handler's return... It puts back the frame...") but it sits directly above `static i64 sys_poll(...)`. The actual sys_sigreturn is at 398 with no such block. The comment migrated away from its function during edits. Harmless but misleading.

3. **"ELF32" stale mentions (confirmed drift, code is ELF64).** The brief flags the README calling the loader "ELF32". In-tree: `elf.c` is genuinely ELF64 (checks ident[4]==2). selftest.c:567 has a leftover comment "Builds a minimal but structurally valid **ELF32** header" immediately followed by the corrected comment "A minimal but well formed **ELF64** executable" -- the two comments contradict each other; the code (`make_elf`, buf[4]=2) is ELF64. So the ELF32 wording is pure stale comment, not a code bug.

4. **`ring3check.py` docstring miscount (confirmed, cosmetic).** ring3check.py:1 says "The **six** things a program can now do" then lists "Floating point, a heap, JavaScript, making another process, and handing it a descriptor" (five) and immediately says "All **five** are ring 3". The SUITES list actually has 19 entries. Internal contradiction in the docstring; the code runs all 19.

5. **`sys_kill` has no ownership check (confirmed, acknowledged in comment).** syscall.c:318-334: any task may kill any other task except itself -- "there is no parent to check against yet." A ring-3 program can `kill()` an unrelated process (e.g. another user's app, or a kernel service task like "net"/"usb", since sys_kill only checks `state != TASK_DEAD`, not `user`). `signal_end_task` on a kernel task would release its fds/windows/sockets and mark it DEAD, stopping that service. This is a real privilege gap, called out as a known limitation rather than hidden. Same applies to `sys_sigsend` (signal_send validates only the sig range and that the target isn't dead -- no owner check), so a program can SIGKILL any pid.

6. **exec leaks the new address space on `user_build_stack` failure (confirmed, minor).** sys_exec (syscall.c:254-257): if `elf_load` succeeds but `user_build_stack` fails, it does `paging_free_directory(dir); return -1;` -- correct. But note: at that point `argv_drop(&a)` was already called at 256 before the `if (!built)` check at 257, so argv is freed -- fine. Actually reviewing closely, the cleanup is correct. The one real asymmetry: on a *successful* elf_load but failed build_stack, the *old* address space is untouched and the process continues (exec returns -1), which is the intended "exec says no rather than losing the process" behaviour (forktest.c:150 checks exactly this). Not a bug.

7. **`winsrv_surface` rollback uses the wrong unmap variant (confirmed, latent bug).** winsrv.c:244-246: when mapping surface pages into a *named* directory `dir`, the failure rollback calls `unmap_page(base + back)` (which operates on `current_pml4`) instead of `unmap_page_in(dir, base + back)`. If `dir` is ever not the live directory at that moment, the rollback unmaps from the wrong address space. In practice sys_win_surface passes `paging_current_directory()` so dir == current and it's benign today, but it's inconsistent with free_slot (winsrv.c:148) which correctly uses `unmap_page_in(s->dir, ...)`. Worth noting for anyone who later maps surfaces into a non-current directory.

8. **`sys_poll` copies pollfds with a plain struct copy after validating only the base range.** syscall.c:366-367: `set[i] = ((const pollfd_t *)r->rbx)[i]` -- the whole `n*sizeof(pollfd_t)` range was validated at 360, so this is safe; noting it because it reads user memory directly rather than via a byte-copy helper (acceptable since the range check covers it).

9. **Reaper bound is a fixed 4096 iterations** (sched.c:537) and `signal_take_pending`/`signal_interrupt` likewise cap ring walks at 4096 (signal.c:98,272). A machine with >4096 tasks would silently skip some -- but next_pid/heap limits make that effectively unreachable; documented as defensive.

10. **`fd_poll` latency**: up to one tick (10 ms) because it re-polls rather than using wait queues (fd.c:450 comment is explicit). Not a bug, a stated trade-off; relevant to anyone expecting sub-tick poll wakeups.

No bugs found in the fork/COW/exec core, the signal frame validation, or the pointer validators -- those are carefully written and well tested.

---

## 11. Open questions

1. Is the unguarded `sys_kill`/`sys_sigsend` (any pid, including kernel service tasks) intended to be tightened before the lead engineer builds multi-user features? It's the biggest security gap in this area.
2. `USER_MMAP_MAX == USER_CODE_BASE` (both base+0x40000000): the mmap window's ceiling is exactly the program image base. A program linked at 0x8040000000 with a large image could in principle collide with the top of the mmap window; is there any check that the ELF image plus mmap don't overlap? (elf.c allows segments up to base+0x4FF00000, well above the image base, so an unusual ELF could load into the stack region -- only entry and per-segment ranges are bounded, not overlap with stack/mmap.)
3. `winsrv_surface` rollback (finding #7) -- confirm no future caller passes a non-current dir.
4. The "forty-seven system calls" vs "sixty-three" wording: which is the intended canonical number for docs going forward (live calls = 63, table slots = 64)?
5. `signal_return`'s rflags mask is `0x0CD5` -- worth confirming this is the deliberate set of user-settable flags (CF,PF,AF,ZF,SF,DF,OF + bit... ) and not missing/including anything (it force-ORs 0x202 = IF + reserved bit 1).
