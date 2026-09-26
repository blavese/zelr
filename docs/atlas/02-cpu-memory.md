# Atlas 02: CPU, interrupts, memory, SMP and platform

Source tree: the repository root (KERNEL_VERSION "0.37.0", include/types.h:25). Everything here comes from reading the code. Where the real selftest boot log (a local `bash run.sh -T` log: QEMU i440fx, `-m 64`, one CPU, multiboot, 556 passed / 0 failed) confirms a derived number, that is noted as **[log]**.

File:line references are to the files as they exist in the tree. Line counts are true line counts, blank lines included.

---

## 1. Scope

| File | Lines | Role |
|---|---:|---|
| include/gdt.h | 56 | Selector constants, per-CPU TSS selector macro `GDT_TSS(cpu)`, TSS API |
| include/idt.h | 59 | `registers_t` interrupt frame, vectors `VEC_AP_WAKE`/`VEC_YIELD`/`VEC_LOCAL_TIMER`, handler registration |
| include/io.h | 56 | Inline port I/O, `io_wait`, `interrupts_enabled`, rdmsr/wrmsr, cpuid, rdtsc, cli/sti/hlt |
| include/pic.h | 11 | 8259 API |
| include/lapic.h | 46 | Local APIC API (enable, id, EOI, timer calibration/start) |
| include/ioapic.h | 37 | IOAPIC API (init, route, mask, GSI lookup) |
| include/smp.h | 98 | `cpu_t`, SMP_MAX_CPUS, AP work-handing API, spinlock API |
| include/acpi.h | 106 | `acpi_info_t` and the parsed MADT/MCFG/FADT structures |
| include/paging.h | 122 | PTE flags, layout constants (64 MiB low map, 64 GiB cap, user half), paging API |
| include/pmm.h | 53 | Frame allocator + per-frame share counts (for COW) |
| include/heap.h | 8 | kmalloc/kcalloc/kfree |
| include/fpu.h | 52 | FPU/SSE policy essay + fxsave API |
| include/timer.h | 6 | PIT API |
| include/types.h | 25 | Integer typedefs, KERNEL_NAME/AUTHOR/YEARS/LICENSE/VERSION |
| include/string.h | 12 | mem*/str* prototypes |
| include/printf.h | 11 | kputc/kprintf/kformat/panic |
| include/serial.h | 14 | COM1 API |
| include/rtc.h | 37 | CMOS clock API, `rtc_time_t` |
| include/rng.h | 41 | RNG API |
| include/power.h | 19 | S5 power-off / reboot API |
| include/pci.h | 62 | PCI config access (ports + ECAM), enumeration, capability walk |
| kernel/gdt.c | 138 | GDT build (5 fixed + 16 two-slot TSS descriptors), 16 TSSs |
| kernel/gdt_flush.S | 35 | `lgdt` + segment reload via `lretq`; `ltr` |
| kernel/idt.c | 289 | IDT build; `isr_dispatch` (big-lock door, COW, fault policy, EOI, task switch, signals) |
| kernel/idt_flush.S | 7 | `lidt` |
| kernel/isr.S | 424 | 52 entry stubs, `isr_common`, `isr_stub_table` |
| kernel/pic.c | 47 | 8259 remap to 32..47, mask/unmask/EOI/disable |
| kernel/lapic.c | 148 | LAPIC map/enable/id/EOI, timer calibration against PIT, periodic timer |
| kernel/ioapic.c | 173 | IOAPIC discovery, mask-all, legacy IRQ routing with MADT overrides |
| kernel/smp.c | 424 | AP bring-up (INIT/SIPI/SIPI, trampoline patch), per-CPU table, spinlocks, work handing, AP idle loop |
| kernel/acpi.c | 336 | RSDP (handoff or scan), XSDT/RSDT, MADT, MCFG, FADT parsing |
| kernel/paging.c | 575 | 4-level paging: identity map, map/unmap/translate, address spaces, fork COW, PAT/WC, device maps |
| kernel/pmm.c | 185 | Bitmap frame allocator (next-fit), reservation, COW share counts |
| kernel/heap.c | 79 | First-fit doubly linked block heap with coalescing |
| kernel/fpu.c | 110 | Enable SSE (CR0/CR4), fxsave/fxrstor, blank FXSAVE image |
| kernel/timer.c | 82 | PIT channel 0 at 100 Hz, tick handler fan-out, `sleep_ms` |
| kernel/divide.c | 35 | libgcc 64-bit division helpers (unused on x86-64) |
| kernel/string.c | 112 | memset/memcpy/memmove/memcmp (8-byte wide), str* |
| kernel/printf.c | 181 | Formatter (console or buffer sink), `panic` |
| kernel/serial.c | 91 | COM1 38400 8N1, IRQ4 receive ring, polled transmit |
| kernel/rtc.c | 194 | CMOS RTC read (double read, BCD, 12/24h), formatting |
| kernel/rng.c | 228 | SHA-256 pool RNG: RDSEED/RDRAND/rdtsc/RTC at init, timer jitter per tick |
| kernel/power.c | 174 | DSDT `_S5_` scan, ACPI S5 power-off, 3-way reboot |
| kernel/pci.c | 257 | CF8/CFC and ECAM config access, brute-force enumeration, capability lists |
| tools/smpcheck.py | 134 | `-smp 4` harness: programs run on APs, APs have their own clocks |
| tools/powercheck.py | 65 | q35 harness: `shutdown` powers the VM off, sleep type read from AML |
| **Total** | **5454** | |

Outside this scope but read for the cross-references this area needs: kernel/sched.c (big-lock implementation, `scheduler_switch`, `pick_next`, `sched_adopt_ap`, `sched_start`), include/sched.h (the lock contract, TASK_STACK_SIZE), kernel/main.c (init order, heap placement and sizing), bootloader/trampoline.S (AP entry), boot/boot.S (boot page tables, CR0), linker.ld, include/handoff.h, uefi/loader.c (memory-type folding), kernel/fb.c (the one real consumer of the AP work API), kernel/sysfs.c (/sys/cpu, /sys/memory), kernel/syscall.c, user.c, elf.c, signal.c and winsrv.c (paging API users), kernel/selftest.c, tools/harness.py, tools/ring3check.py, pipeline/gate.sh, README.md.

---

## 2. Big picture

### 2.1 What this area is
It is the machine layer under everything else: descriptor tables, the interrupt path and its big kernel lock, interrupt controllers (8259, LAPIC, IOAPIC), ACPI discovery, bringing up the other processors, physical and virtual memory, the kernel heap, FPU state, the two clocks (PIT tick, CMOS wall clock), entropy, COM1, formatted output and panic, string primitives, PCI configuration space, and power-off/reboot.

### 2.2 The main design decisions and the reasons the comments give

1. **No higher half. The kernel is identity mapped** (paging.h:25-32, paging.c:14-21). Every kernel pointer is a physical address, so drivers hand pointers straight to DMA engines, and page tables can be edited through their physical addresses without a recursive map. The kernel is linked at 16 MiB (linker.ld:34) and built `-mcmodel=small` (build.sh:46). PML4 entry 0 (the low 512 GiB) belongs to the kernel and is shared by every address space by copying the entry. User space gets PML4 entry 1 (512 GiB to 1 TiB, `USER_SPACE_BASE`/`USER_SPACE_END`, paging.h:62-63). The stated reason: sharing a top-level entry shares everything below it, so a program inside entry 0 would have its tables grafted onto the kernel's (paging.h:49-61).
2. **The low 64 MiB is mapped with 4 KiB pages, RAM above it with 2 MiB pages, and only where the firmware says RAM is** (paging.h:29-47, paging.c:455-492). Reasons: 4 KiB pages everywhere would cost 32 MiB of tables for 16 GiB. Holes are left unmapped so that MMIO gets mapped uncached on demand by `paging_map_device`, because a WB mapping of registers "does not work". The ceiling of 64 GiB is what a 2 MiB bitmap can describe (main.c:62-65).
3. **Bitmap PMM directly after the kernel image, heap after that** (pmm.c:1-11, main.c:53-152). The heap base moved from a fixed 8 MiB to "after the image plus 2 MiB of bitmap room" when the image (which embeds every user program) approached 8 MiB. Heap size is "a quarter, to half a gigabyte" with a 24 MiB floor sized for the compositor's two full-screen buffers (main.c:72-93).
4. **One interrupt path for everything, and the scheduler switches tasks by returning through another task's frame** (isr.S:13-56, idt.c:254-258, sched.c:1-5). Every vector saves the same 15 GPRs plus vector and error code, calls `isr_dispatch`, and `iretq`s through whatever frame pointer comes back.
5. **One big kernel lock**, held by a CPU "whenever it is not executing ring 3 code" (sched.h:164-184, smp.h:16-24, idt.c:148-158). Reason: locking the heap, task list, filesystem and every driver is "not one change but forty". Kernel tasks stay on the BSP (sched.c:423-427). APs run only user programs plus arithmetic handed to them by the compositor.
6. **Kernel built without SSE/x87** (`-mno-sse -mno-sse2 -mno-mmx -mno-80387`, build.sh:45), so vector registers belong only to ring 3 and move only at task switches. The switch is an eager FXSAVE/FXRSTOR on every switch (fpu.h:4-30, sched.c:493/522). There is no lazy switching, no XSAVE, and no AVX state.
7. **Every gate is an interrupt gate**, so syscalls run with IF=0 and "no timer lands in the middle of one" (idt.c:156-158, sched.h:180-183). That is the basis for claiming the kernel is non-preemptive. It holds only for interrupt and syscall paths: kernel tasks run with IF=1 (sched.c:207). See §6 and §10.
8. **PCI through both CF8/CFC and ECAM**; ECAM wins when MCFG exists, because extended config space (offset ≥ 256) is unreachable through the ports (pci.h:4-20, pci.c:1-8).
9. **IOAPIC routing is decided once, after all drivers have registered handlers**, so only claimed lines get routed (main.c:583-605).
10. **Entropy is mixed, never chosen**, and `rng_ready()` gates key generation (rng.h:4-19). TLS refuses to make a key without it (tls.c:601).

---

## 3. File-by-file detail

### 3.1 include/types.h (25)
`u8..u64`, `i8..i64` from `<stdint.h>`. Also pulls in `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>` from the freestanding compiler headers. Defines `KERNEL_NAME "zelr"`, `KERNEL_AUTHOR "blavese"`, `KERNEL_YEARS "2026"`, `KERNEL_LICENSE "GPL-3.0-or-later"`, `KERNEL_VERSION "0.37.0"` (types.h:12-25). These are printed at boot (main.c:217-226), by `uname`, and in the system info window.

### 3.2 include/io.h (56)
All `static inline`:
- `outb/inb/outw/inw/outl/inl` (io.h:4-22).
- `io_wait()`: writes 0 to port 0x80 (io.h:25).
- `interrupts_enabled()`: pushfq/popq, tests bit 9 (io.h:27-33).
- `rdmsr/wrmsr` (io.h:35-43).
- `cpuid_read(leaf,a,b,c,d)`: always sets ECX=0, so it reads subleaf 0 (io.h:44-47).
- `rdtsc()` (io.h:48-52).
- `cli/sti/hlt` (io.h:54-56).

### 3.3 GDT and TSS: include/gdt.h (56), kernel/gdt.c (138), kernel/gdt_flush.S (35)

**Selectors** (gdt.h:7-10, 28-32):

| Selector | Slot | access | flags | Meaning |
|---|---|---|---|---|
| 0x00 | 0 | -- | -- | null |
| 0x08 `GDT_KERNEL_CODE` | 1 | 0x9A | 0x20 (L) | ring 0, 64-bit code |
| 0x10 `GDT_KERNEL_DATA` | 2 | 0x92 | 0x00 | ring 0 data |
| 0x18 `GDT_USER_DATA` (0x1B = `USER_DATA_SEL`) | 3 | 0xF2 | 0x00 | ring 3 data |
| 0x20 `GDT_USER_CODE` (0x23 = `USER_CODE_SEL`) | 4 | 0xFA | 0x20 | ring 3, 64-bit code |
| `GDT_TSS(cpu)` = 0x28 + 16·cpu | 5+2·cpu, 6+2·cpu | 0x89 | limit[19:16] | 64-bit available TSS, CPU 0..15 (0x28 … 0x118) |

User data sits before user code "because that is the order sysret would want" (gdt.c:44-47). SYSCALL/SYSRET are not used, though.

**Structures** (gdt.c:19-42): `struct gdt_entry` (8 bytes, packed); `struct gdt_system_entry` (16 bytes: base split over `base_low/mid/high/upper`); `struct gdt_ptr {u16 limit; u64 base}`; `struct tss_entry` (packed, 104 bytes: `reserved0 u32, rsp0, rsp1, rsp2, reserved1, ist[7], reserved2, reserved3 u16, iomap_base u16`).

**Globals**: `gdt[GDT_SLOTS]` with `GDT_FIXED 5`, `GDT_SLOTS = 5 + SMP_MAX_CPUS*2 = 37` entries, so 296 bytes and limit 295 (gdt.c:48-51). `tss[SMP_MAX_CPUS]` is `aligned(64)` (gdt.c:56). Only the array is aligned; see §10 D14.

**Functions**:
- `set_gate(i, access, flags)`: base = limit = 0 (gdt.c:61-68).
- `set_tss(cpu)`: zeroes the TSS, sets `iomap_base = sizeof(tss)` (no I/O bitmap, so ring 3 port I/O takes #GP), and writes the 16-byte descriptor at slot `GDT_TSS(cpu)/8` with limit 103 (gdt.c:89-106).
- `gdt_init()`: builds the four fixed descriptors and all 16 TSS descriptors on the BSP, then `gdt_flush(&gdtp)` and `tss_flush(GDT_TSS(0))` (gdt.c:108-129).
- `gdt_load_cpu(cpu)`: an AP reloads the same table and loads its own task register (gdt.c:131-138).
- `tss_set_stack_for(cpu, rsp0)`, `tss_set_stack(rsp0)` (uses `smp_this_cpu()`), `tss_stack_of(cpu)`, `tss_current_selector()` (`str`) (gdt.c:70-84).
- `gdt_flush` (gdt_flush.S:11-28): `lgdt (%rdi)`, loads 0x10 into ds/es/fs/gs/ss, then pushes 0x08 and a return address and does `lretq` to reload CS (long mode has no far jump to an immediate). `tss_flush`: `ltr %di` (gdt_flush.S:32-35).

**IST**: the `ist[7]` fields are all zero and every IDT gate has `ist = 0` (idt.c:44). No exception has a dedicated stack, so #DF and NMI run on whatever stack was current. Kernel stacks come from the heap and have no guard page. Overflow is caught only by the paint check on the next switch (sched.c:501-502).

**RSP0 flow**: for an AP, `start_cpu` sets rsp0 to the AP's boot-stack top before starting it (smp.c:185-191). After that `scheduler_switch` sets rsp0 = `stack_base + TASK_STACK_SIZE` of the incoming task on every switch (sched.c:526). The BSP's rsp0 stays 0 until the first switch, which is harmless because no ring 3 code runs before then.

### 3.4 IDT, stubs and dispatch: include/idt.h (59), kernel/idt.c (289), kernel/idt_flush.S (7), kernel/isr.S (424)

**Frame** `registers_t` (idt.h:10-15). The stubs push `err_code` (or 0) first and then `int_no`; `isr_common` then pushes rax…r15, so r15 ends up at the lowest address:

| Offset | Field | Offset | Field |
|---:|---|---:|---|
| 0 | r15 | 96 | rcx |
| 8 | r14 | 104 | rbx |
| 16 | r13 | 112 | rax |
| 24 | r12 | 120 | int_no |
| 32 | r11 | 128 | err_code |
| 40 | r10 | 136 | rip (CPU) |
| 48 | r9 | 144 | cs (CPU) |
| 56 | r8 | 152 | rflags (CPU) |
| 64 | rbp | 160 | rsp (CPU, always pushed in long mode) |
| 72 | rdi | 168 | ss (CPU) |
| 80 | rsi | | sizeof = 176 |
| 88 | rdx | | |

Segment registers are not saved and there is no `swapgs`. With 16-byte CPU alignment plus 7 + 15 pushes, rsp is 16-aligned at `call isr_dispatch`. A task that is not running keeps exactly this frame on its own kernel stack. `task_create` and `task_create_user` fabricate one at `top - 176` with `rflags = 0x202` and `int_no = 32` (sched.c:196-209, 331-343). `task_fork` copies the parent's frame.

**Entry and exit** (isr.S:13-56): push the 15 GPRs, `cld`, `rdi = rsp`, `call isr_dispatch`, `mov %rax,%rsp` (possibly another task's frame), pop the 15 GPRs, `add $16,%rsp`, `iretq`.

**Stubs** (isr.S:58-365): `isr0`–`isr47`, `isr128`, `isr240`, `isr241`, `isr242`, 52 in total. Stubs 8, 10, 11, 12, 13, 14, 17, 21, 29 and 30 rely on the CPU-pushed error code. All the others push 0. `isr_stub_table` (isr.S:369-424) is in that order: indices 0..47, then 48 = isr128, 49 = isr240, 50 = isr241, 51 = isr242.

**Vector map** (idt.c:53-76 plus the handler registrations):

| Vector | Gate | Source | Handling |
|---|---|---|---|
| 0–31 | 0x8E, sel 0x08, IST 0 | CPU exceptions | no handler registered (the selftest registers #3, selftest.c:358). #PF gets COW first, then demand fill. Ring 3 fault → `end_the_program` → `task_exit_with(139)`. Ring 0 → `bb_fault` + `panic` (idt.c:199-237) |
| 32 | 0x8E | IRQ0, PIT (**[log]** "irq0 arrives on input 2" under the IOAPIC) | `timer.c` `on_tick`, then `scheduler_switch` (idt.c:256) |
| 33 | 0x8E | IRQ1 keyboard | keyboard.c:166 |
| 36 | 0x8E | IRQ4 COM1 | serial.c:66 |
| 44 | 0x8E | IRQ12 PS/2 mouse | mouse.c:284 |
| 32+irq | 0x8E | NIC INTx (config-space interrupt line) | e1000.c:290, pcnet.c:368, rtl8139.c:163 |
| other 32–47 | 0x8E | unrouted, or 8259 spurious 39/47 | no handler; EOI is still sent (idt.c:243-245) |
| 0x80 | 0xEE (DPL 3) | `int 0x80` syscall | `syscall_handler` (syscall.c:1089-1101): rax = number, args rbx/rcx/rdx, result in rax |
| 0xF0 `VEC_AP_WAKE` | 0x8E | IPI from `smp_run` | answered before the lock: `lapic_eoi(); return r;` (idt.c:164) |
| 0xF1 `VEC_YIELD` | 0x8E | `task_yield` → `int $0xF1` (sched.c:612) | `scheduler_switch` only; no tick is counted (idt.h:31-44) |
| 0xF2 `VEC_LOCAL_TIMER` | 0x8E | AP LAPIC periodic timer | `smp_note_tick`, try-lock, `scheduler_switch` |
| 0xFF | **not present** | LAPIC spurious vector (SVR, lapic.c:48) | would raise #NP (§10 B6) |
| all others | not present | -- | #NP (vector 11) if raised |

Tables: `static struct idt_entry idt[256]`, `idt_ptr idtp`, and `isr_handler_t handlers[256]` (idt.c:20-34). `register_interrupt_handler(n,h)` has no locking (idt.c:49). `idt_has_handler(n)` is used by IOAPIC routing (idt.c:51, main.c:594). `idt_load()` is called by each AP (idt.c:78, smp.c:259). `EXC[32]` holds the exception names (idt.c:80-89). `from_user(r)` is `(r->cs & 3) == 3` (idt.c:94).

**`isr_dispatch(registers_t *r)`, step by step** (idt.c:147-289):
1. `VEC_LOCAL_TIMER` → `smp_note_tick(smp_this_cpu())` (163).
2. `VEC_AP_WAKE` → `lapic_eoi()` and return the same frame. It never touches the lock (164).
3. **Lock door** (166-188): if this CPU does not hold the lock, `VEC_LOCAL_TIMER` uses `kernel_lock_try()`. On a miss it calls `smp_note_lock_miss`, `lapic_eoi`, and returns without a switch. Any other vector calls `kernel_lock_acquire()`, which spins with IF=0.
4. **COW first** (199-204): `int_no == 14` and `(err & 3) == 3` (present plus write, user bit deliberately not required) → `paging_resolve_cow(paging_current_directory(), cr2)`. On success, **return immediately**.
5. Registered handler → call it (206).
6. Otherwise, for exceptions (<32): read cr2 on #PF. If it came from ring 3 and `user_fault_fill(cr2, err)` succeeds (demand paging for mmap/stack/brk), **return immediately** (220-221). Otherwise `end_the_program` for ring 3, or `bb_fault` + `panic` for ring 0 (226-236).
7. EOI (243-252): vectors 32–47 get `lapic_eoi()` when the IOAPIC is active, else `pic_eoi(irq)`. `VEC_LOCAL_TIMER` and `VEC_AP_WAKE` get `lapic_eoi()`. The EOI happens before the switch, which is safe because IF=0.
8. `int_no` of 32, `VEC_YIELD` or `VEC_LOCAL_TIMER` → `resume = scheduler_switch(resume)` (255-258).
9. If the frame being returned through is ring 3, call `signal_deliver(back)` (285).
10. Release the lock if `from_user(back) || task_is_idle(task_current())` (287).

### 3.5 8259: include/pic.h (11), kernel/pic.c (47)
Ports 0x20/0x21 and 0xA0/0xA1 (pic.c:6-9). `pic_init()` saves the masks, sends ICW1 0x11, ICW2 0x20 and 0x28, ICW3 0x04 and 0x02, ICW4 0x01 (8086 mode, normal EOI), with `io_wait` between writes, then **restores the saved masks** (pic.c:11-25). `pic_eoi(irq)`: slave then master for irq ≥ 8 (27-30). `pic_disable()`: masks everything, 0xFF to both (32-35). `pic_mask`/`pic_unmask`: read-modify-write of the data port (37-47). Drivers call `pic_unmask` directly even when the IOAPIC is in use. That is harmless once `pic_disable` has run and the IOAPIC routes the line.

### 3.6 Local APIC: include/lapic.h (46), kernel/lapic.c (148)
Registers (lapic.c:8-20): ID 0x020, SVR 0x0F0, EOI 0x0B0, LVT timer 0x320, initial count 0x380, current count 0x390, divide 0x3E0. `LVT_PERIODIC 0x20000`, `LVT_MASKED 0x10000`, `DIV_16 0x3`.
- `lapic_init()` (30-50): idempotent. Calls `acpi_init()`, maps `acpi()->lapic_base & ~0xFFF` with `paging_map_device(…,0x1000)` (UC), and sets SVR |= 0x100 | 0xFF (enable, spurious vector 0xFF). It does not check the IA32_APIC_BASE MSR, has no x2APIC support, and does not touch TPR or LINT0/LINT1.
- `lapic_enable()` (59-62): the same SVR write, run by each AP for itself (smp.c:247).
- `lapic_id()`: `ID >> 24` (8-bit xAPIC ID), or 0 before mapping (64-66). `lapic_present()`, `lapic_regs()`.
- `pit_count()` (94-99): latch command 0x00 to port 0x43, then read channel 0 low and high from port 0x40.
- `lapic_timer_calibrate()` (101-134): only if mapped and not yet calibrated. Divide 16, LVT masked, initial count 0xFFFFFFFF. Polls `pit_count()` until it has seen 5 "wraps" (`now > prev`), with a guard of 200,000,000 iterations. Then `ticks_per_second = (0xFFFFFFFF - current) * (hz/5)` with hz = `timer_hz()` (100). The result is set to 0 if the loop timed out or the value is below `hz*100`. Called once from `smp_init` after `lapic_init` and before any AP starts (smp.c:400). **The PIT runs in mode 3, so every period contains two wraps.** See §10 B5.
- `lapic_timer_start(vector)` (136-143): divide 16, LVT = vector | periodic, initial count = `ticks_per_second / hz`. Only APs call it (smp.c:292). The BSP's LAPIC timer stays masked and stopped after calibration.
- `lapic_eoi()`: writes 0 to EOI (146-148).

### 3.7 IOAPIC: include/ioapic.h (37), kernel/ioapic.c (173)
Window registers IOREGSEL +0x00 and IOWIN +0x10. Registers: ID 0x00, VER 0x01, redirection entries at `0x10 + 2n`. Low-half bits: fixed delivery, physical destination, `POLARITY_LOW` (1<<13), `TRIGGER_LEVEL` (1<<15), `ENTRY_MASKED` (1<<16) (ioapic.c:17-29). `chip_t {regs, gsi_base, inputs}`, `chips[ACPI_MAX_IOAPIC=4]`, `nchips`, `ready` (31-39).
- `ioapic_init()` (96-132): requires `lapic_init()` and an MADT that lists IOAPICs. Maps each one UC with 0x1000 bytes, reads VER bits 16..23 as the last input, skips chips claiming ≥ 240 inputs, and masks every entry. Called from main.c:590 after all drivers are initialised. **[log]**: "ioapic, 24 inputs, 5 routed, 8259 masked".
- `write_entry(c,input,low,high)` (73-80): masks first, writes the high half, then the low half. The stated reason is that the two halves cannot be written atomically.
- `ioapic_gsi_for_irq(irq)`: the override GSI, else irq (82-87). `override_for(irq)` (89-94).
- `ioapic_route_irq(irq, vector)` (134-155): low = vector, fixed, physical, plus polarity and trigger **only from an override**; otherwise the ISA defaults (edge, active-high). The destination high half is `lapic_id() << 24`, i.e. the caller's CPU (the BSP, since main.c:595 calls it). Returns false if no chip owns the GSI.
- `ioapic_mask_irq`/`ioapic_unmask_irq`: read-modify-write of the low half (157-173). `ioapic_inputs()` returns the sum over chips.
- There is no routing for GSIs ≥ 16 and no `_PRT` evaluation. PCI INTx works only through the ISA IRQ number in config offset 0x3C. That works under QEMU; on real chipsets in APIC mode it may not.

Routing policy (main.c:590-605): if `ioapic_init()` succeeds, `pic_disable()` runs when `acpi()->has_8259` (MADT PCAT_COMPAT). Then every IRQ n in 0..15 whose vector 32+n has a handler is routed to 32+n.

### 3.8 SMP: include/smp.h (98), kernel/smp.c (424), plus bootloader/trampoline.S (111)
**Types**. `cpu_t` (smp.h:32-50): `u8 apic_id; bool started; volatile u32 jobs; volatile u64 spins; volatile u64 user_slices; volatile u64 local_ticks; volatile u64 lock_misses`. `slot_t` (smp.c:59-64): `{cpu_t info; void (*volatile fn)(void*); void *volatile arg; u64 stack_base}`. `static slot_t cpus[SMP_MAX_CPUS=16]` (smp.c:66). `ncpus` (the BSP is entry 0, then usable APs in MADT order), `nstarted` (initially 1), `lapic`, `active` (91-94).

**Constants**: `TRAMPOLINE_PHYS 0x8000` (39); `AP_STACK_SIZE = TASK_STACK_SIZE = 32768` (44; sched.h:32). ICR flags: INIT 0x500, STARTUP 0x600, ASSERT 0x4000, LEVEL 0x8000, PENDING 0x1000 (53-57). LAPIC ICR_LO 0x300, ICR_HI 0x310 (50-51).

**Identity**. `smp_this_cpu()` (84-90) returns 0 if there is no LAPIC. Otherwise it reads the LAPIC ID (an MMIO read) and linearly searches `cpus[]` for a **started** entry with that ID, falling back to 0. An AP therefore reads as CPU 0 until it sets `started` (smp.c:279). There is no GS base or other per-CPU pointer: every `cur()`, lock operation and `tss_set_stack` does this MMIO read and search.

**Spinlocks** (106-117): `spin_lock` is `__sync_lock_test_and_set` (xchg) with an inner read-spin plus `pause`. `spin_try` tries once. `spin_unlock` is `__sync_lock_release`. Interrupts are not disabled.

**Delays**: `delay_us(us)` does `us/15+1` transitions of port 0x61 bit 4 (the ~15.085 µs refresh toggle), each with a 100,000-read guard (137-145). `apic_wait()` polls ICR delivery status for up to 1,000,000 reads (129-132).

**Bring-up** (`smp_init`, 371-424; called at main.c:488 after `sched_init()` at main.c:486):
1. Clear `cpus[]`, `ncpus = nstarted = 1`, `active = false`. Return if `acpi_init()` fails or there are no CPUs, or if `lapic_init()` fails.
2. `lapic_timer_calibrate()`, which needs the PIT to itself.
3. `cpus[0] = {apic_id = lapic_id(), started = true}`. Append every MADT CPU that is not self and has `usable` set (enabled or online-capable), up to 16.
4. If `ncpus < 2`: `active = true` and return.
5. `memcpy(0x8000, trampoline_start, len)` (417-418), then `start_cpu(i)` for each AP. `nstarted++` on success.

`start_cpu(i)` (170-220):
- `kmalloc(32 KiB)` for the stack, then `sched_paint_stack`, record `stack_base`, top = aligned end.
- `tss_set_stack_for(i, top)`.
- `patch_trampoline(top, i)` finds `"ZELRSMP1"` in the copy at 0x8000 and writes 4 quads: CR3 = `paging_kernel_directory()`, stack, entry = `ap_main`, arg = index (152-168).
- INIT assert (`INIT|ASSERT|LEVEL`), INIT de-assert (`INIT|LEVEL`), 10 ms delay.
- Up to 2 × {SIPI `STARTUP | 0x08` (no ASSERT bit), 200 µs, then poll `started` 200 × 1 ms}. The second SIPI is sent only if the first attempt timed out.
- A failure leaks the stack. Only the patch-failure path frees it.

`trampoline.S`:
- Real mode at 0x8000: `cli; cld`, zero segments.
- CR4.PAE on; CR3 = **32-bit** load of `param_cr3` (so the kernel PML4 must be below 4 GiB, which holds because it is the PMM's first allocation, ~1 MiB); EFER.LME on.
- CR0 |= PG|PE in a single write (it **only ORs**, so CD/NW/WP stay as INIT left them); `lgdt` with its own 3-entry GDT (0x08 code, 0x10 data); `ljmpl` to 64-bit code.
- Load segments with 0x10, `rsp = param_stack`, `rdi = param_arg`, `call *param_entry`.

`ap_main(index)` (238-318), never returns:
- `lapic_enable()`; `gdt_load_cpu(index)`; `idt_load()`; `fpu_init()`; `paging_init_pat()`; `started = true`.
- Under the kernel lock: `sched_adopt_ap(index, stack_base)`, which makes the current stack this AP's idle task and inserts it into the task ring (sched.c:560-585).
- `lapic_timer_start(VEC_LOCAL_TIMER)`.
- Idle loop: `cli`; if `me->fn` is set: `sti`, read `arg`, clear `me->fn`, run `fn(arg)` **without the lock and with IF=1**, `jobs++`. Otherwise `spins++; sti; hlt`.

**Afterwards**, an AP is a scheduling CPU. On each `VEC_LOCAL_TIMER` for which it wins the try-lock, `scheduler_switch` → `pick_next`:
- returns the AP's idle task if a job is pending (sched.c:439);
- never picks kernel tasks (sched.c:464);
- skips tasks running on another CPU (sched.c:463).

`sched_note_user_slice` counts user slices (smp.c:76-78, sched.c:521).

**Work-handing API** (smp.c:320-367). The one production user is fb.c's band comparison (fb.c:440-470):
- `smp_helper()`: the first started AP with `fn == 0`, or 0. It does **not** check whether that AP is idle or running a program.
- `smp_run(cpu, fn, arg)`: refuses cpu 0, an AP that is not started, or one that is busy. Writes `arg`, `__sync_synchronize`, then `fn`, then sends a fixed IPI `ASSERT | VEC_AP_WAKE` to its APIC ID.
- `smp_busy(cpu)` and `smp_work_pending(cpu)` test `fn != 0`.
- `smp_wait(cpu, ms)` spins with `pause` until `fn == 0` or a PIT-tick deadline. `fn` is cleared **when the job is picked up**, not when it finishes (fb.c:356-360 knows this and uses its own `helper_done` flag).

Getters: `smp_cpu_count()` (= ncpus), `smp_started()`, `smp_cpu(i)`, `smp_active()`, `smp_note_tick`, `smp_note_lock_miss`.

### 3.9 ACPI: include/acpi.h (106), kernel/acpi.c (336)
Limits: `ACPI_MAX_CPUS 16`, `ACPI_MAX_MCFG 4`, `ACPI_MAX_IOAPIC 4`, `ACPI_MAX_OVERRIDE 16` (acpi.h:22-25).

`acpi_info_t` (acpi.h:69-95):
- `found` (= ncpus > 0), `lapic_base` (default 0xFEE00000), `ncpus`, `apic_id[16]`, `usable[16]`, `oem[7]`
- `revision`, `used_xsdt`, `ntables`
- `acpi_fadt_t fadt {present, pm1a_cnt, pm1b_cnt, smi_cmd, acpi_enable, dsdt}`
- `nmcfg`/`mcfg[4] {base, segment, start_bus, end_bus}`
- `nioapic`/`ioapic[4] {address, gsi_base, id}`
- `noverride`/`override[16] {source, gsi, active_low, level_triggered}`
- `has_8259`

Flow (`acpi_init`, 274-336; idempotent through `acpi_ran`; first called at main.c:379, after paging):
1. `find_rsdp()` (107-129): first the handoff pointer from `acpi_use_rsdp(h->rsdp)` (main.c:378; non-zero only under UEFI), accepted if the signature and the 20-byte v1 checksum match. Otherwise scan the first 1 KiB of the EBDA (`*(u16*)0x40E << 4`, only if within 0x400..0xA0000), then 0xE0000–0xFFFFF on 16-byte boundaries.
2. Use the XSDT if `revision ≥ 2`, `length ≥ 36`, the extended checksum is OK and `xsdt_address != 0`. Otherwise use the RSDT. Fall back to the RSDT if the XSDT signature is wrong (292-313). Reject a directory whose length is outside 36..64 KiB or whose checksum fails.
3. `read_table(phys)` for **every** entry (216-229): map the header, check the length is within 36..0x10000, map the whole table, verify the checksum, then dispatch on `APIC`, `MCFG`, `FACP`.
   - `read_madt` (131-186): `lapic_base` from the header; `has_8259 = flags & 1`. Type 0 LAPIC: `apic_id = p[3]`, `usable = flags & 3`. Type 1 IOAPIC: id `p[2]`, address `p+4`, GSI base `p+8`. Type 2 override: source `p[3]`, gsi `p+4`, flags `p+8`; active-low if polarity bits = 3, level if trigger bits = 3. Type 5: LAPIC address override, low 32 bits only. Types 9/10 (x2APIC) are **ignored**.
   - `read_mcfg` (188-210): 16-byte entries after 8 reserved bytes; keeps those with `base != 0 && start ≤ end`.
   - `read_fadt` (247-266): requires length ≥ 90. DSDT from offset 40, replaced by X_DSDT at offset 140 when revision ≥ 2 and length ≥ 148. SMI_CMD at 48, ACPI_ENABLE at 52, PM1a_CNT_BLK at 64, PM1b_CNT_BLK at 68. `present = pm1a_cnt != 0`. The X_PM1x GAS fields and the hardware-reduced sleep registers are not read.
- `map_phys(phys,len)` (72-85) returns the pointer directly if both ends already translate to themselves. Otherwise it maps missing 4 KiB pages **WB** (`PTE_PRESENT|PTE_RW`) into the current directory.

### 3.10 Paging: include/paging.h (122), kernel/paging.c (575)

**Flags** (paging.h:5-23):

| Name | Value | Meaning |
|---|---|---|
| `PTE_PRESENT` | 0x001 | |
| `PTE_RW` | 0x002 | |
| `PTE_USER` | 0x004 | |
| `PTE_NOCACHE` | 0x018 | PWT\|PCD, PAT index 3 = UC |
| `PTE_WC` | 0x080 | 4 KiB PTE PAT bit, PAT index 4, reprogrammed to WC |
| `PTE_HUGE` | 0x080 | PS bit in a PD/PDPT entry (**same bit as PTE_WC**) |
| `PTE_COW` | 0x200 | software bit 9: read-only because shared |
| `PTE_ADDR_MASK` | 0x000FFFFFFFFFF000 | |

NX is never used (EFER.NXE is never set), and neither is the global bit.

**Layout constants**: `KERNEL_LOW_MB 64`; `KERNEL_SPACE_MAX_GB 64`; `USER_SPACE_BASE 0x0000008000000000`; `USER_SPACE_END 0x000000FFFFFFF000`.

**Globals**: `kernel_pml4`, `current_pml4` (paging.c:32-33; **a single global, not per CPU**, see §10 B1); `cow_shared`, `cow_copies` (249); `mapped_bytes` (445); `pat_ready` (534).

**Internal helpers**:
- `index_of(v, level)`: `(v >> (12 + 9·(level-1))) & 0x1FF` (35-38).
- `step(table, v, level, create, flags)` (41-62): allocate and zero a missing table, which then gets `PRESENT|RW`. OR in `PTE_USER` whenever `flags` has it, so directory entries widen on user mappings. Return 0 on a huge entry.
- `split_huge(pd,i)` (75-88): converts a 2 MiB PDE into 512 4 KiB PTEs with the same flags, dropping PS.
- `table_for` (91-106): splits a huge PDE when `create`, returns 0 when not creating.
- `map_huge_in` (109-117).
- `resolve` (123-142): translates through 1 GiB, 2 MiB or 4 KiB leaves.
- `map_in` (144-151): writes the PTE and does `invlpg` **only if `pml4 == current_pml4`**.

**Public API**:
- `map_page(v,p,f)` / `map_page_in(dir,v,p,f)` (153-160); `unmap_page(v)` / `unmap_page_in(dir,v)` (170-179). Unmapping clears the PTE; `invlpg` only if `dir == current_pml4`; page tables are never freed.
- `virt_to_phys(v)` / `virt_to_phys_in(dir,v)` (415-422). Returns 0 for "unmapped", which is ambiguous for physical address 0.
- `virt_is_user_in(dir,v)` (427-439): `PRESENT|USER` at every level. It **does not check RW**.
- `paging_new_directory()` (186-196): a new zeroed PML4 with every present kernel PML4 entry copied (in practice only entry 0). `KERNEL_PML4_ENTRIES 1` at line 184 is defined but unused.
- `paging_clone_directory(src)` (346-386): new directory. Kernel-shared entries (same table address as `kernel_pml4[i]`) are copied as is; others go through `copy_table(…,3)`. Afterwards, reload CR3 if `src == current_pml4`, to flush the parent's now read-only entries.
- `copy_table` (254-290): allocates fresh tables for levels > 1. Huge and non-user leaves are copied verbatim. User leaves go through `pmm_share(frame)`; on success **both** parent and child PTEs become `(e & ~RW) | COW` and `cow_shared++`; otherwise it copies eagerly with `memcpy` through the identity map.
- `paging_resolve_cow(dir, addr)` (318-344), via `entry_for` (303-316, a 4 KiB leaf only):
  - if the PTE has `PTE_COW` and `pmm_holders(frame) ≤ 1`: set RW and clear COW in place;
  - else allocate, copy, `pmm_free_frame(frame)` (drops one holder), install the new frame with RW and without COW, `cow_copies++`;
  - always `invlpg(page)`.
- `paging_free_directory(dir)` (388-404): skips `kernel_pml4` and kernel-shared entries. `free_table` (203-216) frees every table and **every `PTE_USER` leaf frame** through `pmm_free_frame`, skipping bit-7 leaves. Finally frees the PML4.
- `paging_switch(dir)`: `current_pml4 = dir; mov cr3` (406-410). `paging_current_directory()` returns the global; `paging_kernel_directory()` (412-413).
- `paging_init(h)` (448-504):
  1. The PML4 comes from `pmm_alloc_frame()` (first free frame, typically 0x100000).
  2. Identity-map [0, 64 MiB) with 4 KiB pages `PRESENT|RW` (including page 0).
  3. For each MEM_USABLE region, round **inward** to 2 MiB, clamp to [64 MiB, 64 GiB), and map missing 2 MiB pages `PRESENT|RW`.
  4. Load CR3. No #PF handler is registered here; the single fault path is in idt.c.
  **[log]**: "paging enabled, 64 MiB mapped" on a 64 MiB VM.
- PAT (527-549): `IA32_PAT 0x277`. `PAT_WITH_WC 0x0007040100070406` = slots WB, WT, UC-, UC, **WC**, WT, UC-, UC; only slot 4 changes from the power-on default. `paging_init_pat()` checks CPUID.1:EDX[16] and writes the MSR; run on the BSP (main.c:348) and on every AP (smp.c:277). `paging_wc_ready()`. **[log]** "write combining".
- `paging_map_wc(phys,bytes)` (551-565): without PAT, falls back to `paging_map_device`. Otherwise, for each page, `unmap_page` then `map_page(PRESENT|RW|PTE_WC)`. Used for the framebuffer (fb.c:133).
- `paging_map_device(phys,bytes)` (567-575): for each page, skips it if `virt_to_phys(a) == a` (**whatever its cache type**), else maps `PRESENT|RW|PTE_NOCACHE`. Identity address, into the **current** directory; these land in the shared PML4[0] tables unless the address is ≥ 512 GiB. Callers: lapic.c:41, ioapic.c:108, pci.c:74, power.c:84/90, ahci.c:256, e1000.c:278, hda.c:408, nvme.c:257, svga.c:183-184, xhci.c:503.

### 3.11 PMM: include/pmm.h (53), kernel/pmm.c (185)
**State** (pmm.c:20-23): `u32 *bitmap` (one bit per 4 KiB frame; 1 = used), `bitmap_bytes`, `total_frames`, `used_frames`. `next_hint` (114). `u8 *extra` (139): the per-frame count of additional holders, from the heap. `shared_now` (148).

**`pmm_init(h, bitmap_limit)`** (51-106):
1. `highest` = the maximum end of any usable region (64 MiB if none), capped at 64 GiB.
2. `bitmap = page_align(__kernel_end)`; `room = bitmap_limit - bitmap`; cap `highest` at `room·8·4096`.
3. `total_frames = highest/4K`. Fill the bitmap with 0xFF (all used), free page-aligned frames inside usable regions below `highest`, re-mark [0, 1 MiB) used, and mark from `__kernel_start` to the page-aligned end of the bitmap used.

`bitmap_limit` is `heap_base()` (main.c:327-328).

- `pmm_reserve(start,size)` marks a range used. Used once, for the heap (main.c:336).
- `pmm_alloc_frame()` (116-127): **next-fit**. Scans from `next_hint`, wraps once, marks the frame used, sets `next_hint = i+1`. Returns a physical address, or 0 when exhausted. Frame 0 is never free, so 0 unambiguously means failure. Allocations sweep through all memory over time, because frees never move `next_hint` back.
- `pmm_free_frame(a)` (172-181): if `extra[f] > 0`, decrement it (and `shared_now` when it reaches 0) and **do not free**; else `mark_free`.
- Sharing: `pmm_share_init()` (`kcalloc(total_frames)`, main.c:358); `pmm_share_ready()`; `pmm_share(a)` refuses at 255 extra holders; `pmm_holders(a)` returns 1 + extra.
- Accounting: `pmm_total_frames()` (includes holes below `highest`, e.g. the 3–4 GiB PCI hole), `pmm_used_frames()`, `pmm_free_frames() = total - used`. Reported by /sys/memory (sysfs.c:96-106), the boot banner (main.c:329-332, 362-365), the shell (shell.c:184) and sysinfo (syscall.c:1003). `pmm_shared_frames()` is never reported anywhere.

### 3.12 Heap: include/heap.h (8), kernel/heap.c (79), sizing in kernel/main.c
`block_t {u32 size (payload bytes); bool free; block_t *next, *prev}`. `HDR = sizeof(block_t) = 24`, `MIN_SPLIT 16` (heap.c:6-13). Blocks are physically contiguous and the list is in address order. There are no footers, despite the "boundary tags" comment.
- `heap_init(start,size)`: one free block (18-25).
- `kmalloc(n)` (39-51): returns 0 for n == 0. Rounds `n = (n+7) & ~7u`, **a 32-bit mask**. First fit from `head`, `split` when the remainder can hold `HDR + 16`. Returns payload = block + 24 (8-aligned).
- `kcalloc(n)`: kmalloc then `memset(p,0,n)` (53-57).
- `kfree(p)` (59-76): ignores NULL and already-free blocks; marks free, merges forward then backward.
- `heap_used()`, `heap_total()`: u32 counters. `used` counts `size + HDR`.
- No locking, no per-CPU caches, no validation of the pointer passed to `kfree`.

**Placement and sizing** (main.c:53-152):
- `heap_base() = align_up_1MiB(__kernel_end + BITMAP_ROOM(2 MiB))`.
- `heap_size_for`: `need = HEAP_MIN (24 MiB) + align_1MiB(2 × pitch×4×height)` (UEFI framebuffer only; `fb_pitch` is in pixels, handoff.h). `want = max(usable_total/4 (HEAP_SHARE 4), need)`, capped at `HEAP_CEIL` 512 MiB, capped at the contiguous usable run from base minus 1 MiB. If it crosses 64 MiB, the end is truncated to a 2 MiB boundary.
- **[log]**: -m 64 gives "heap 24 MiB at 0x1C00000" (so `__kernel_end` ≈ 0x1983000, ~25.5 MiB); "53 MiB usable" before the heap, "29 MiB left for programs" after.

### 3.13 FPU: include/fpu.h (52), kernel/fpu.c (110)
`FPU_AREA 512`. `fpu_init()` (fpu.c:40-66): CR0 clears EM, sets MP, clears TS; CR4 sets OSFXSR (bit 9) and OSXMMEXCPT (bit 10); `fninit`; `ready = true`. The `ready` flag is one global even though every CPU calls `fpu_init`. `fpu_save`/`fpu_restore`: `fxsave`/`fxrstor` when ready (70-78). `fpu_blank(area)` (87-110): zeroed image with FCW 0x037F, **abridged FTW byte = 0xFF**, MXCSR 0x1F80, MXCSR_MASK 0xFFFF.

The policy is eager save and restore on every `scheduler_switch` (sched.c:493, 522), including kernel-to-kernel switches. The task's area is `task_t.fpu[FPU_AREA+16]`, aligned up to 16 at use (sched.c:53-56, sched.h:67). CR0.TS is never set, so there is no #NM lazy scheme. CR4.OSXSAVE is never set, so there is no XSAVE and AVX would raise #UD in ring 3. The kernel itself is compiled `-mno-sse -mno-sse2 -mno-mmx -mno-80387`.

### 3.14 PIT timer: include/timer.h (6), kernel/timer.c (82)
`ticks` (volatile u64) and `frequency = 100` (timer.c:13-14).

`timer_init(hz)` (46-54): divisor = 1193182/hz = 11931 for 100 Hz, which is 100.0015 Hz. Command **0x36 (channel 0, lo/hi, mode 3 square wave)**. Registers vector 32 and does `pic_unmask(0)`. Called at main.c:459.

`on_tick` (16-44), on the BSP only, in this order:
1. `ticks++`
2. `usb_poll()` (xHCI is polled, not interrupt-driven)
3. `ps2_poll_from_timer()`
4. `syn_tick()`
5. `sound_poll()`
6. `rng_tick()`

`isr_dispatch` then calls `scheduler_switch`, so the tick also drives preemption, sleep and wait deadlines (sched.c:446-457, wait.c:43), the reap grace period (sched.c:71, `REAP_GRACE = 1000` ticks) and `smp_wait` deadlines.

`timer_ticks()`, `timer_hz()`. `sleep_ms(ms)` (76-82): `delta = ceil(ms·hz/1000)`, at least 1, then `while (ticks < target) hlt`. It **requires IF=1 and a ticking BSP**; with IF=0 it never returns.

### 3.15 divide.c (35)
`udivmod64` is a bitwise restoring divider; division by zero returns 0 with remainder 0. Wrappers: `__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3`, `__udivmoddi4`. x86-64 compilers emit `div` for 64-bit operands, so nothing references these (confirmed by grep). The header rationale is from the 32-bit era.

### 3.16 string.c (112)
- `memset` (15-31): byte loop up to 8-byte alignment, then u64 stores of `0x0101010101010101*b`, then a byte tail.
- `memcpy` (33-50): aligns the destination, then u64 copies (the source may be unaligned), then a tail.
- `memmove` (52-64): forward through `memcpy` if `dp < sp` or there is no overlap, else a byte-wise backward copy.
- `memcmp` (73-88): u64 compare when both pointers share alignment, then byte-wise for the sign.
- `strlen`, `strcmp`/`strncmp` (unsigned compare), `strcpy`, `strncpy` (zero-pads), `strchr` (finds the NUL when c == 0).

**The "volatile trick"**: the first header comment (string.c:3-6) says volatile pointers stop clang's loop-idiom pass from turning these loops into self-recursive calls. The code no longer has volatile; the second comment (7-14) says it was removed as useless. What protects the functions now is `-ffreestanding -fno-builtin` (build.sh:42-44). README:1423-1426 still says the opposite (§10 D3).

### 3.17 printf.c (181)
- Sinks: console (`kputc`: `fbcon_putc` if `fb_active()`, else `vga_putc`; always `serial_putc`, with `\r` before `\n`) or a buffer (`sink_t {buf,cap,len}`).
- Supported: flag `-`, then flag `0` (in that order only), a decimal width, and conversions `%d %i` (i32), `%u %x %X %b` (u32), `%p` (full 64-bit as `0x` plus 16 digits), `%c`, `%s` (NULL prints "(null)"), `%%`. Unknown conversions print `%` plus the character without consuming an argument.
- Not supported: precision, length modifiers, 64-bit integers except through `%p`. Callers cast to u32 everywhere; grep finds no `%l`/`%z`/`.N` in kernel/.
- `kvformat`/`kformat` always NUL-terminate and return the length wanted.
- `panic` (157-181): `cli`, red colours, the message to the console and `bb_log`, `bb_flush` (disk first), `bb_screen`, `hlt` forever. Only the calling CPU stops; other CPUs keep running ring 3 until they need the lock.

### 3.18 serial.c (91)
- `COM1 0x3F8`. `serial_init` (11-19): IER 0; DLAB; divisor 3 (38400); 8N1; FCR 0x07 (enable and clear FIFOs, 1-byte trigger); MCR 0x0B (DTR|RTS|OUT2).
- RX ring `rx[RXSZ=512]` with `rx_head`/`rx_tail` and counters `rx_overruns`, `rx_isr_bytes`, `rx_read_bytes`, `rx_isr_calls` (24-33).
- `serial_isr` (35-63): up to 64 rounds while IIR shows an interrupt pending. LSR bit 1 counts overruns; drain while LSR bit 0; **Ctrl-C (0x03) calls `signal_interrupt()`** and is also queued; a full ring counts an overrun.
- `serial_enable_irq()` (65-69): handler on 36, IER = 1, `pic_unmask(4)`. Called at main.c:548, before IOAPIC routing, so IRQ4 gets routed.
- `serial_trygetc()` (73-79) is consumed by keyboard.c:192.
- TX is polled on LSR bit 5 with no timeout (81-88). `serial_has_input`/`serial_getc` are raw polled reads.

### 3.19 rtc.c (194)
- CMOS index 0x70, data 0x71. Registers: sec 0, min 2, hour 4, weekday 6, day 7, month 8, year 9, A 0x0A, B 0x0B. Status bits: A.UIP 0x80, B.24h 0x02, B.binary 0x04.
- `cmos_read` writes `0x80|reg`, which leaves NMI masked, as intended (45-53).
- `rtc_init` (78-101): if both A and B read 0xFF, there is no clock. Latch `binary_mode` and `hour24`, one `rtc_read`, then range sanity checks. `present` becomes true.
- `rtc_read` (103-158): wait for UIP to clear (1,000,000-spin bound, returns false on timeout), sample, then up to 8 re-samples until two consecutive samples agree (the last one is used even if they never agree). Convert BCD, preserving the PM bit. Convert 12-hour to 24-hour. Year < 70 means 20xx, else 19xx; the century register is never read.
- `rtc_format` produces "YYYY-MM-DD hh:mm:ss" and needs cap ≥ 20. `rtc_format_short` produces "hh:mm". Both write "no clock" or "--:--" on failure.
- Read-only: no timezone, never written.
- Users: main.c:450, sysfs.c:278, wm.c:2152 (panel clock), x509.c:172 (certificate validity), rng.c:147.

### 3.20 rng.c (228)
- `pool[32]` holds a SHA-256 state; `counter`; `have_hardware`, `have_rdseed`, `have_rdrand`; jitter state `jitter_last`, `jitter_spread`, `jitter_samples`; `JITTER_ENOUGH 128`.
- `stir_locked(d,len)`: `pool = SHA256(pool || d)`. `rng_stir` wraps it in cli/sti when IF was set (82-95).
- `rng_init` (133-170): zero everything. Stir rdtsc, then the RTC time. If CPUID.7:EBX[18], up to 16 × `rdseed` (32 retries each). If CPUID.1:ECX[30], up to 16 × `rdrand`. Called at main.c:466, after `rtc_init` and `timer_init`, while interrupts are still off.
- `rng_tick` (108-123), in the PIT interrupt: `delta = rdtsc - last`; stir `delta` (except for the very first zero sample); `spread |= delta ^ (delta>>17)`; `samples` saturates at 256.
- `rng_ready()` = `have_hardware || (samples ≥ 128 && (spread & 0xFFFF) != 0)` (127-129, 172).
- `rng_sources()` builds "rdseed+rdrand+timing" or "none" on demand (178-194).
- `rng_bytes(out,len)` (196-228): per 32-byte block, snapshot pool and counter with IF off, output `SHA256(snapshot || counter)`, then stir `1` and rdtsc so the pool moves on.
- **[log]**: "no rdseed and no rdrand, timing jitter only" on the default QEMU CPU. The selftest waits up to 4 s for readiness.

### 3.21 power.c (174)
- `SLP_EN = 1<<13`, `SCI_EN = 1<<0`. State: `looked`, `have_s5`, `slp_typ_a`, `slp_typ_b` (27-32).
- `find_s5` (75-102), lazy: needs `fadt.present` and `fadt.dsdt`. Maps the DSDT header **UC** with `paging_map_device(…,64)`, reads the length (36..1 MiB), maps the whole table UC, scans bytes from offset 36 for `_S5_`, then `decode_s5(p+4)` or `decode_s5(p+5)`. It does not check for a preceding NameOp and does not look in SSDTs.
- `decode_s5` (42-73): expects PackageOp 0x12, a PkgLength whose top 2 bits count the extra length bytes, NumElements ≥ 2, then two elements, each `0x0A byte` (BytePrefix), `0x00` or `0x01`. Anything else is refused.
- `power_can_off()`: `have_s5 && fadt.present`. `power_describe()`.
- `power_off()` (145-174):
  1. If `smi_cmd && acpi_enable` and SCI_EN is clear, write `acpi_enable` to `smi_cmd` and poll SCI_EN up to 300 × `sleep_ms(10)`.
  2. `bb_log("powering off, sleep type a/b")` (powercheck.py looks for "sleep type").
  3. `outw(pm1a, SLP_TYPa<<10 | SLP_EN)`, and the same for pm1b if it exists.
  4. `sleep_ms(500)`; return false.
- `power_reboot()` (130-143): wait ≤ 100,000 reads for 8042 input-buffer empty, then `0xFE` to port 0x64; `0x02` then `0x06` to port 0xCF9; `lidt` with a zero limit plus `int3` to triple fault; `hlt` loop.
- Callers: shell.c:574-584 (`shutdown`/`poweroff`/`reboot`, kernel task, IF=1), syscall.c:463-475 `sys_power` (IF=0), wm.c:2979.

### 3.22 pci.c (257)
`pci_dev_t {bus, slot, func, vendor, device, bar0 (raw register), irq (config 0x3C)}` (pci.h:22-27). BAR decoding, 64-bit BARs and sizing are left to drivers (xhci.c:492-493, nvme.c:252-253 and hda.c:402-403 read the high dword; the others assume 32-bit).

- ECAM state: `ecam_base`, `ecam_first_bus`, `ecam_last_bus`, `ecam_ready`, and a `bus_mapped[8]` bitmap (21-31).
- `pci_ecam_init()` (33-55): takes the first MCFG entry with segment 0. Called at main.c:380. **[log]** i440fx: "pcie none, using the legacy config ports".
- `ecam_at` (64-83) rejects buses out of range, slot > 31, func > 7 and offset > 0xFFF, then aligns the offset to 4. On the first access to a bus it maps that bus's 1 MiB with `paging_map_device` (UC).
- Port path: `0x80000000 | bus<<16 | slot<<11 | func<<8 | (off & 0xFC)` through 0xCF8/0xCFC.
- `pci_read32` tries ECAM first. Without ECAM, offsets ≥ 256 read as 0xFFFFFFFF and writes to them are dropped.
- `pci_read16` extracts a word. `pci_write16` is a 32-bit read-modify-write.
- `last_bus()`: `ecam_last_bus` or 255.
- `pci_find(vendor,device)` is brute force over bus × 32 × 8 and honours the multifunction bit. `pci_find_class(class,sub,progif)` and `pci_list_class(class,sub,out,max)` (the latter returns the total, which may exceed `max`) scan all 8 functions without checking the multifunction bit.
- `pci_enable_bus_master` sets I/O, memory and bus-master bits through the 16-bit read-modify-write.
- `pci_find_cap` walks the legacy capability list, bounded to 48 entries. `pci_find_ext_cap` walks from 0x100, ECAM only. **Neither has callers** outside pci.c.

---

## 4. Control flow and lifecycles

### 4.1 Boot order (kernel/main.c:301-646; everything runs on the BSP with IF=0)
1. `serial_init`, `bb_init`, `vga_init`, handoff magic check, command line (`selftest`, `console`).
2. `gdt_init` (GDT, 16 TSSs, `ltr 0x28`); `idt_init` (gates 0–47, 0x80, 0xF0–0xF2); `pic_init` (remap to 32–47, masks restored).
3. `heap_base()`, then `pmm_init(h, hbase)`, then `pmm_reserve(hbase, heap_size_for(h,hbase))`.
4. `fpu_init` (BSP).
5. `paging_init(h)` (new tables, CR3 switch), then `paging_init_pat`.
6. `heap_init(hbase, heap_bytes)`, then `pmm_share_init()`.
7. `acpi_use_rsdp(h->rsdp)`, `acpi_init()`, `pci_ecam_init()`.
8. Video, filesystem, disk, built-in programs.
9. `rtc_init()`; `timer_init(100)`; `rng_init()`.
10. `sched_init()` must come before `smp_init()`, because APs adopt themselves into the task ring (main.c:479-486).
11. `smp_init()`: `lapic_init` (maps the LAPIC), `lapic_timer_calibrate`, then APs start, run `ap_main`, adopt idle tasks, and arm local timers. Their ticks now take the try-lock, and `scheduler_switch` returns early because `started == false`.
12. Network, input drivers (these register vectors 33, 44, and 32+NIC IRQ), USB, sound, `serial_enable_irq` (vector 36).
13. `ioapic_init()`; `pic_disable()` if PCAT_COMPAT; route every claimed IRQ 0–15 to 32+n on the BSP.
14. `syscall_init` (vector 0x80), window server, service tasks, the `init` or `selftest` task.
15. `sched_start()` creates the BSP idle task, **acquires the kernel lock**, sets `started = true`, `sti`, and loops on `hlt`. The first vector-32 tick switches into a task; the boot context is discarded because `cur()` is NULL.

### 4.2 Interrupt entry and exit
Stub (push err/0, push vector) → `isr_common` (push 15 GPRs) → `isr_dispatch(frame)`, steps as in §3.4 → returns an rsp → pop, `add 16`, `iretq`. A task switch simply returns a different task's saved `rsp`, which points at its `registers_t`. `scheduler_switch` (sched.c:479-558), in order:
1. `signal_take_pending()`.
2. Save `cur()->rsp` and FXSAVE.
3. Paint check at `stack_base` (panics on overflow).
4. RUNNING → READY; `on_cpu = -1`.
5. `pick_next`.
6. `set_cur`, `on_cpu = me`, RUNNING, `slices++`, user slice count.
7. FXRSTOR.
8. `tss_set_stack(stack_base + 32 KiB)`.
9. CR3 switch **if `want != paging_current_directory()`**.
10. Reap dead tasks: `paging_free_directory`, then kfree the stack and the record.
11. Return the new rsp.

### 4.3 Big kernel lock lifecycle (implemented in sched.c:90-118)
`static spinlock_t kernel_lock; static volatile int lock_holder = -1`.
- `kernel_lock_acquire()`: `spin_lock`, then `lock_holder = smp_this_cpu()`.
- `kernel_lock_try()`.
- `kernel_lock_release()`: `lock_holder = -1`, then unlock.
- `kernel_lock_held_here()`: `lock_holder == smp_this_cpu()`.

| Event | Site | Effect |
|---|---|---|
| Scheduler start | sched.c:600 | BSP takes it once, before the first `sti` |
| AP adoption | smp.c:285-287 | acquire, `sched_adopt_ap`, release |
| Interrupt/syscall/fault entry while not held | idt.c:166-188 | `VEC_LOCAL_TIMER`: try-lock or skip; everything else: blocking acquire with IF=0 |
| Entry while held (kernel task on the BSP, nested fault in a syscall) | idt.c:166 | nothing |
| `VEC_AP_WAKE` | idt.c:164 | never involved |
| Exit | idt.c:287 | release if the frame returned through is ring 3, or the current task after the switch is an idle task |
| Resolved COW fault, or demand-filled fault, from ring 3 | idt.c:203, 221 | **returns to ring 3 still holding the lock** (§10 B2) |

Consequences, which follow from the design:
- A kernel task on the BSP holds the lock for its whole slice, including while it halts in `task_idle_wait` (wm.c:4060, shell.c:614) or `sleep_ms`.
- APs enter the kernel only while the BSP runs a user program or its idle task.
- A syscall that blocks (`wait_on` → `int 0xF1`) keeps holding the lock through the switch until the next frame chosen is ring 3 or idle. Whichever CPU later resumes the blocked task already holds the lock from its own entry.
- The BSP's vector 32 blocks on the lock like any other vector, so PIT ticks are delayed or lost while an AP holds the lock (§10 B27).

### 4.4 AP bring-up sequence
`smp_init`: calibrate → build the CPU list → copy the trampoline to 0x8000. Then, serially for each AP: `start_cpu` (stack, TSS rsp0, patch parameters, INIT assert/de-assert, 10 ms, SIPI, 200 µs, poll ≤ 200 ms, maybe a second SIPI) → trampoline (PAE, CR3, LME, PG|PE, GDT, long mode, stack, `ap_main(index)`) → `ap_main` (LAPIC enable, own TSS, IDT, FPU, PAT, `started = true`, adopt the idle task under the lock, local timer, idle loop).

### 4.5 Page-fault handling order (idt.c)
1. Present + write → `paging_resolve_cow(current_pml4, cr2)`.
2. Registered handler (none in production).
3. Ring 3, not present → `user_fault_fill(cr2, err)` (user.c:286; stack growth and VMA fill).
4. Ring 3 → `end_the_program`: one `bb_log` line, then `task_exit_with(139)`.
5. Ring 0 → `bb_fault`, then panic with rip, err, cs, rflags, rsp and cr2.

### 4.6 Fork, copy-on-write and exit
- `sys_fork` (syscall.c:198-215) → `paging_clone_directory(parent->dir)`: user pages become read-only + COW in both directories, `extra[frame]++`, and the parent's TLB is flushed if its directory is `current_pml4`.
- A ring 3 write → #PF(present|write|user) → resolve: copy, or re-enable RW if it is the last holder.
- Exit: `task_exit_with`, then the reaper runs `paging_free_directory`, and `pmm_free_frame` on each user leaf drops a holder or frees the frame.
- Exec (syscall.c:228-302) builds a new directory, switches, then frees the old one.

### 4.7 Timekeeping and preemption
- PIT (BSP): vector 32 → `ticks++` plus polling fan-out → `scheduler_switch`. This is the only time base.
- APs: `VEC_LOCAL_TIMER` counts `local_ticks` and switches tasks when it wins the try-lock. It never touches `ticks`.
- `VEC_YIELD`: switch only.

### 4.8 Power
- Shutdown: `find_s5` (lazily maps and scans the DSDT) → optional SMI ACPI-enable → PM1a/PM1b writes → returns false if the machine did not turn off.
- Reboot: 8042 → CF9 → triple fault.

---

## 5. Interfaces

### 5.1 Exports and their main consumers
- **Frame and handlers**: `registers_t` is used by sched.c (frame build, fork copy), syscall.c (argument ABI, exec rewrites the frame), signal.c (`signal_deliver(back)`, sigreturn), idt.c, and blackbox (`bb_fault`). `register_interrupt_handler` is called from timer.c, keyboard.c, mouse.c, serial.c, e1000.c, pcnet.c, rtl8139.c, syscall.c and selftest.c. `idt_has_handler` from main.c:594.
- **GDT**: `USER_CODE_SEL`/`USER_DATA_SEL`/`GDT_KERNEL_*` (sched.c:204-205, 338-339; syscall.c:292-293); `tss_set_stack` (sched.c:526); `tss_set_stack_for` (smp.c:191); `tss_current_selector`/`tss_stack_of` (selftest.c:1178-1219).
- **Paging**:
  - `map_page_in`/`unmap_page_in`/`virt_to_phys_in` in user.c, elf.c and winsrv.c.
  - `virt_is_user_in` in syscall.c:61-65 and signal.c:162-167.
  - `paging_new_directory`/`paging_free_directory`/`paging_clone_directory` in syscall.c (exec, fork) and user.c.
  - `paging_switch` in sched.c:529, syscall.c:265, user.c:280.
  - `paging_current_directory` in idt.c:202, syscall.c:61/943, signal.c:163, winsrv.c:292, selftest.
  - `paging_map_device`/`paging_map_wc` in drivers (§3.10).
  - `paging_kernel_directory` in smp.c:161 and sched.c:528.
- **PMM**: `pmm_alloc_frame`/`pmm_free_frame` in paging.c, user.c:57-61/276/310-338, elf.c:92-96. Statistics go to sysfs, the shell, syscall sysinfo and main.
- **Heap**: 137 call sites across 30 kernel files (grep count), among them winsrv (surfaces), fb (back buffers), sched (task records and stacks), tls, fat, vfs, usb and net.
- **SMP**: `smp_this_cpu` (sched.c `cur()` and the lock, gdt.c `tss_set_stack`, idt.c); `smp_helper`/`smp_run` (fb.c:442-450); `smp_work_pending` (sched.c:439); `smp_cpu*` (sysfs.c:137-151 `/sys/cpu`, shell.c:485-495, syscall.c:999-1000); spinlocks (selftest).
- **LAPIC/IOAPIC/PIC**: `lapic_eoi` (idt.c); `ioapic_*` and `pic_disable` (main.c); `pic_unmask` (drivers, timer, serial); `lapic_timer_hz` (sysfs.c:141, "apictimer N counts per second").
- **ACPI**: `acpi()` in lapic.c, ioapic.c, smp.c, pci.c, power.c, main.c, selftest.c.
- **Timer**: `timer_ticks` is everywhere (sched.c, wait.c, smp.c, drivers, sysfs uptime); `sleep_ms` in power.c, sound.c:280, usbnet.c:120, wait.c:35, selftest.
- **RNG**: tls.c:601-608 (refuses without `rng_ready`). **RTC**: x509.c:172, wm.c:2152, sysfs.c:278. **Serial**: keyboard.c:192 (`serial_trygetc`), shell.c:189 (statistics). **printf**: all over. **power**: shell.c, syscall.c `sys_power`, wm.c:2979. **PCI**: drivers (§3.22), main.c device survey, netdev.c, wifi.c.

### 5.2 Dependencies of this area
- sched.c/sched.h: the lock, `task_current`, `task_exit_with`, `task_is_idle`, `sched_adopt_ap`, `sched_paint_stack`, `TASK_STACK_SIZE`, `scheduler_switch`.
- user.c (`user_fault_fill`), signal.c (`signal_deliver`, `signal_interrupt`), blackbox.c (`bb_log`/`bb_fault`/`bb_flush`/`bb_screen`), sha256.c (rng), the polling hooks in timer.c (usb, ps2, synaptics, sound).
- vga/fb/fbcon (`kputc`), handoff.h, bootloader/trampoline.S, linker symbols `__kernel_start`/`__kernel_end`.

---

## 6. Concurrency, locking, memory ownership, invariants

1. **The big kernel lock is the only cross-CPU exclusion.** The heap, PMM, page tables, task ring, ACPI/PCI state, the `handlers[]` table and the CF8/CFC port pair have no locks of their own and rely on it. Allowed without the lock: the `VEC_AP_WAKE` path, the per-CPU counters in `cpus[]`, the AP idle loop including handed functions (fb band comparisons touch only caller-owned buffers), and idle tasks.
2. **Same-CPU preemption is not excluded.** Kernel tasks run with IF=1 (sched.c:207) and vector 32 calls `scheduler_switch` whatever the interrupted CPL (idt.c:256). Two kernel tasks on the BSP can therefore interleave inside `kmalloc`/`kfree` or any other unlocked structure. The "non-preemptive kernel" wording holds only for interrupt and syscall context (IF=0). Specific structures use ad-hoc cli/sti (wait.c, winsrv.c:349, net/tcp, sound, rng). The heap and PMM do not. See §10 B9.
3. **Interrupt-context versus task-context sharing on the BSP**: `rng` pool writes use cli/sti in task context and `stir_locked` inside the tick. The tick's USB, PS/2 and sound polling runs inside an interrupt that may have landed in the middle of a kernel task using the same driver.
4. **Per-CPU state** is found through `smp_this_cpu()`: the TSS, `current_of[]`, `idle_of[]`, `lock_holder`, and `cpus[]`. The paging module's "current directory" is **not** per CPU (§10 B1).
5. **Memory ownership**:
   - Frames belong to whoever `pmm_alloc_frame` handed them to. User leaves (`PTE_USER`) are assumed PMM-owned by the address space, which is why `free_table` frees them.
   - Kernel heap pages are reserved in the PMM and must never reach `pmm_free_frame`. Window surfaces break this assumption, because they are heap pages mapped with `PTE_USER`. winsrv unmaps them on task exit (winsrv.c:148, called from sched.c:672), but exec and fork do not special-case them (§10 B16).
   - Shared (COW) frames carry an `extra` count; the last `pmm_free_frame` actually frees them.
   - Kernel page tables under PML4[0] are never freed.
6. **Invariants relied upon**:
   - Every frame the PMM hands out is identity mapped. It is dereferenced through its physical address immediately (user.c:59, elf.c:94, paging.c:47, 265, 286, 332). **This is violated** by partial 2 MiB chunks above 64 MiB (§10 B4).
   - The kernel PML4 is below 4 GiB, because the trampoline does a 32-bit CR3 load.
   - Kernel tasks never run on an AP (sched.c:464).
   - A task runs on at most one CPU at a time (`on_cpu`, sched.c:463).
   - Every kernel stack is 32 KiB and painted with `0xC5…`; the bottom word must still carry the paint at each switch (sched.c:501).
   - EOI must be sent for every vector 32–47, `VEC_LOCAL_TIMER` and `VEC_AP_WAKE`, or delivery at that priority stops (idt.c:239-252).
   - Redirection entries are written masked first (ioapic.c:73-80).
   - The trampoline is patched and used by one AP at a time; startup is serialised by the `started` handshake.
7. **TLB maintenance** is local only: `invlpg` on the calling CPU, or a CR3 reload. There are no shootdown IPIs. That is acceptable today only because an address space belongs to exactly one task and the only unmaps in the shared kernel region happen at boot, before the APs start (`paging_map_wc`). Future unmap or permission-reduction code must add shootdowns.

---

## 7. Limits and magic numbers

| Constant | Value | Where | Meaning / consequence |
|---|---|---|---|
| Kernel link/load address | 16 MiB (0x1000000) | linker.ld:34, uefi/loader.c:388 | must be inside the 4K-mapped low 64 MiB |
| `KERNEL_LOW_MB` | 64 | paging.h:33 | 4 KiB identity map [0,64 MiB), including page 0 |
| `KERNEL_SPACE_MAX_GB` | 64 | paging.h:47 | cap for 2 MiB mapping and for the PMM |
| `BITMAP_ROOM` | 2 MiB | main.c:65 | 1 bit per 4 KiB frame covers 64 GiB |
| `HEAP_MIN` / `HEAP_SHARE` / `HEAP_CEIL` | 24 MiB / 1/4 / 512 MiB | main.c:81, 92, 93 | kernel heap sizing |
| `USER_SPACE_BASE`..`END` | 0x80_0000_0000..0xFF_FFFF_F000 | paging.h:62-63 | PML4 entry 1 |
| User sub-layout | image window base..+0x4FF00000 (programs linked at 0x8040000000); heap +0x10000000..+0x38000000; mmap +0x38000000..+0x40000000; stack top +0x50000000 (max 1 MiB); surfaces +0x60000000, 8 MiB each | elf.c:42-43, user.h:38-109, winsrv.h:19-20 | owned by the process area |
| `PAT_WITH_WC` | 0x0007040100070406 | paging.c:532 | slot 4 = WC |
| `SMP_MAX_CPUS` / `ACPI_MAX_CPUS` | 16 / 16 | smp.h:30, acpi.h:22 | CPUs beyond 16 MADT entries are ignored; xAPIC IDs only |
| `ACPI_MAX_IOAPIC` / `MCFG` / `OVERRIDE` | 4 / 4 / 16 | acpi.h:23-25 | |
| ACPI table max length | 64 KiB (DSDT: 1 MiB) | acpi.c:219, 316; power.c:88 | |
| `TRAMPOLINE_PHYS` | 0x8000 | smp.c:39 | SIPI vector 0x08 |
| AP/task kernel stack | 32768 | sched.h:32, smp.c:44 | the deepest selftest path used 16416 bytes **[log]** |
| Boot stack | 64 KiB in .bss | boot.S:67-69 | BSP until `sched_start` |
| INIT delay / SIPI wait | 10 ms / 200 µs + 200×1 ms, ×2 | smp.c:202, 211-216 | ~400 ms maximum per AP |
| `delay_us` granularity | 15 µs (port 0x61 bit 4) | smp.c:137-145 | guard of 100,000 reads per transition |
| LAPIC SVR spurious vector | 0xFF | lapic.c:48 | no IDT gate |
| LAPIC divide | 16 (`0x3`) | lapic.c:18 | |
| LAPIC calibration | 5 PIT wraps, guard 2·10^8 | lapic.c:113-115 | minimum rate `hz*100`; PIT mode 3 doubles the wrap count |
| PIT | 1193182 Hz / 11931 → 100 Hz, mode 3 (cmd 0x36) | timer.c:48-51 | |
| `REAP_GRACE` | 1000 ticks (10 s) | sched.c:71 | |
| IDT vectors | 0–47, 0x80, 0xF0, 0xF1, 0xF2 | idt.c:59-73 | |
| GDT slots | 37 (296 bytes) | gdt.c:48-49 | |
| TSS size | 104 bytes, no IOPB | gdt.c:34-42, 94 | |
| Heap header / min split / alignment | 24 / 16 / 8 | heap.c:12-13, 41 | |
| Share count ceiling | 255 extra holders | pmm.c:158 | above that, fork copies |
| Serial | COM1 0x3F8, 38400 8N1, RX ring 512 | serial.c:9-24 | |
| RTC | UIP spin 1e6, 8 re-reads, year pivot 70 | rtc.c:108-153 | |
| RNG | `JITTER_ENOUGH` 128 ticks (1.28 s) | rng.c:31 | |
| S5 | `SLP_EN` bit 13, SLP_TYP << 10, SMI wait 300×10 ms, final 500 ms | power.c:27-28, 157-172 | |
| Reboot | 8042 wait 100,000 reads; CF9 0x02 then 0x06 | power.c:131-137 | |
| PCI | CF8/CFC; ECAM 1 MiB per bus mapped lazily; capability walk bound 48 | pci.c:16-17, 72-76, 235, 250 | |
| Handoff | `HANDOFF_MAX_REGIONS` 128, magic "ZELR64HF" | handoff.h | |

---

## 8. Tests

**Kernel selftest** (kernel/selftest.c, `-append selftest`, order at selftest.c:3548-3605). Counts are from the -m 64, one-CPU run **[log]**:

| Section | Checks | What it verifies |
|---|---:|---|
| `[string]` test_string (75-92) | 8 | strlen/strcmp/strcpy/memset/memcpy/memcmp/memmove overlap |
| `[the identity map]` (116-171) | 2 at -m 64 (up to 7) | mapped ≥ 64 MiB; 1:1 translation every 4 MiB up to 64 MiB. With more than 68 MiB mapped: 2 MiB pages mapped and writable, split of one page, neighbours intact. With more than 4 GiB: 0xC0000000 unmapped |
| `[physical memory]` (173-181) | 4 | allocation, alignment, free count |
| `[paging]` (183-204) | 5 | map/unmap at 13 MiB (inside the heap) and the identity mapping restored |
| `[user access]` (211-237) | 5 | `virt_is_user_in` semantics. **Maps at 0x38000000/0x39000000 in a directory sharing PML4[0]** (§10 B17) |
| `[heap]` (94-112) | 5 | distinct blocks, coalescing to baseline, 512 KiB allocation, kcalloc zeroing |
| `[timer]` (314-349) | 3 | ticks advance; 60 ms is 4–20 ticks; 50 yields add fewer than 10 ticks |
| `[interrupts]` (354-365) | 2 | `int $3` reaches a registered handler, re-entrantly |
| `[randomness]` (1670-1700) | 5 | ready within 4 s, sources named, output non-zero, distinct, no repeated blocks |
| `[processors]` (1182-1289) | 4 on UP; 15 on SMP | BSP uses TSS 0x28 with rsp0 ≠ 0. On SMP: all started, AP1 on TSS 0x38 with its own rsp0 (via `smp_run`), idle AP spins < 100 in 50 ms, 20000×(n) spinlocked increments exact, per-CPU argument, job counts |
| `[acpi and pcie]` (3191-3259) | 4 without MCFG | tables found, directory read, CPUs described; ECAM versus port agreement; aliasing and out-of-range reads |
| `[interrupt routing]` (3271-3334) | 9 with IOAPIC | LAPIC up, ≥ 16 inputs, overrides sane, IRQ0 line known ("input 2" **[log]**), ticks advance, masking stops and unmasking restarts the timer, route of IRQ 200 refused |
| `[clock]` (3392-3483) | 18 | field ranges, 4-digit year, monotonic, running, 4000 reads without going backwards (the comment admits QEMU cannot produce torn reads), formatting |
| `[kernel stack]` (1718-1731) | 2 | headroom ≥ 8 KiB (**[log]**: 16352 of 32768 spare) |

**Harnesses** (pipeline/gate.sh runs `ring3check`, `framecheck`, `powercheck`, `smpcheck`):
- tools/smpcheck.py: `-smp 4`, 256 MiB. Four CPUs listed in `/sys/cpu` before and after; after 6 × `bg /bin/spin`, the BSP and at least one AP have more user slices; each of the 3 APs has more than 200 local ticks; `ps` works; `exec /bin/hello` prints 5050. `spin` makes no syscalls in its loop, so the SMP syscall, COW and lock bugs in §10 are **not exercised**. The tick threshold passes whether the LAPIC rate is 100 Hz or ~250 Hz.
- tools/powercheck.py: q35, `shutdown`. Checks that the log lacks "cannot power off", that the QEMU process exits within 20 s with code 0, and that the log contains "sleep type". Exercises FADT, DSDT/_S5 decode and `power_off` from a kernel task (IF=1).
- tools/framecheck.py: `-smp 2`; frame statistics from `/sys/screen`, including `shared` frames, which exercises `smp_helper`/`smp_run`.
- tools/ring3check.py: 256 MiB, **one CPU**. Runs fptest (SSE in ring 3), cowtest (fork sharing, measured through sysinfo free memory), forktest, faulttest (faults end programs), maptest (demand paging), sigtest and others.
- tools/harness.py `Guest`: defaults to `-m 64`, no `-smp`, i440fx, `-kernel` (multiboot). Most harnesses therefore never see SMP or more than 64 MiB of RAM.
- tools/check_sse.py scans the kernel ELF for SSE opcodes, but **nothing invokes it** (§10 D4).

---

## 9. How to extend (and the pitfalls the comments warn about)

- **A new IRQ-driven device**: register the handler on vector 32+irq **before** main.c:590 so the IOAPIC routing loop sees it (routing happens once). Drivers initialised later must call `ioapic_route_irq(irq, 32+irq)` themselves. PCI devices on GSI ≥ 16 need new routing code and, on real hardware, `_PRT` or MSI (`pci_find_cap` exists but has no callers).
- **A new vector**: add a stub to isr.S, append it to `isr_stub_table`, and `set_gate` it in idt.c:53-76. The stub-table index differs from the vector number for anything past 47. Add EOI handling in idt.c:243-252 if the LAPIC delivers it. Decide whether it must bypass the lock (like `VEC_AP_WAKE`) or may only try it (like `VEC_LOCAL_TIMER`).
- **Per-CPU data**: today it is `array[smp_this_cpu()]`, which costs an MMIO read plus a search per access. A GS-based per-CPU block would help. The current CR3 or directory must become per-CPU first (§10 B1).
- **Finer locking**: the lock door is in `isr_dispatch` and in the early returns. Any new early return must follow the release rule at idt.c:287. Heap and PMM locks would also have to guard against same-CPU preemption between kernel tasks: kernel tasks run with IF=1.
- **Memory map changes**: keep the PMM and the identity map in agreement. Anything the PMM can hand out must be mapped, and whatever `paging_init` leaves unmapped must be reserved. Keep the kernel PML4 below 4 GiB (trampoline). Keep new MMIO below 512 GiB or give it a non-identity window, because identity mapping at 512 GiB or above lands in the user half. `PTE_WC` and `PTE_HUGE` are the same bit; the generic walkers (`free_table`, `copy_table`) treat any leaf with bit 7 as huge.
- **Unmapping shared kernel mappings, or reducing permissions in an address space live on another CPU**, needs a TLB-shootdown IPI mechanism that does not exist yet.
- **SSE/AVX for programs**: switching to XSAVE needs CR4.OSXSAVE, XCR0, a larger per-task area (currently `FPU_AREA 512` + 16 inside `task_t`) and 64-byte alignment. Keep the kernel free of SSE (build.sh flags; wire tools/check_sse.py into build.sh).
- **Power**: hardware-reduced ACPI (FADT flag HW_REDUCED, sleep control/status registers) and the X_PM1x GAS fields would go in `read_fadt` (acpi.c:247-266) and `power_off`. Never call `sleep_ms` with IF=0.
- **Clock**: the comment says setting the RTC needs a timezone database (rtc.h:12-14). Century register use depends on FADT (rtc.c:148-152).
- **printf**: there are no length modifiers. Add `%lu`/`%lx` handling before printing u64 values without casts.

---

## 10. Doc drift and suspicious code (verified by reading; runtime impact not observed)

### 10.1 Likely bugs

- **B1. `current_pml4` is one global, but CR3 is per CPU.** Code throughout treats it as "this CPU's address space" (paging.c:33, 406-413). `paging_switch` on any CPU overwrites it. The BSP's `scheduler_switch` puts back the kernel directory whenever it switches between kernel tasks after an AP loaded a user directory (sched.c:528-529). So on SMP, a program running on an AP usually sees `paging_current_directory()` return the kernel's or another process's directory. Effects:
  1. `user_range_ok` (syscall.c:56-66) validates syscall pointers against the wrong directory. Valid calls spuriously fail with -1. A pointer that is valid only in the other process passes, and the kernel's access then faults in ring 0, which panics.
  2. A COW write fault on an AP is looked up in the wrong directory (idt.c:202). The fault is not resolved, `user_fault_fill` refuses present pages, and the program is killed with 139. Or another process's PTE gets modified.
  3. The signal-stack check (signal.c:162-167) uses the wrong directory.
  4. The fork TLB flush is skipped when `src != current_pml4` (paging.c:384), so the parent keeps writable TLB entries for pages it now shares ("a fork that did not fork").
  5. `map_in`/`unmap_page_in` decide whether to `invlpg` from the global (paging.c:148, 175).
  6. winsrv resize is refused spuriously (winsrv.c:292).
  7. An AP moving to its idle task skips the CR3 reload when `want == current_pml4`, so it keeps a dead task's CR3. The reaper later frees that PML4 (sched.c:549). If the frame is reused while the AP is still idle, the AP's page walks read garbage, which could triple fault.

  Uniprocessor runs (every harness except smpcheck and framecheck) cannot see any of this. Fix: read CR3, or keep `current_pml4[smp_this_cpu()]`.
- **B2. Resolved faults return to ring 3 holding the big lock.** In `isr_dispatch`, the COW success path (idt.c:202-203) and the demand-fill path (idt.c:220-221) `return (u64)r` without reaching the release at idt.c:287. For faults taken in ring 3, the CPU resumes user code with `lock_holder` still set to itself. Other CPUs spin with IF=0, and APs miss their try-locks, until that CPU takes another interrupt. On a uniprocessor the next interrupt quietly repairs it. This contradicts the lock contract at sched.h:166-168 and idt.c:150-152.
- **B3. CR0.WP is never set, so COW is bypassed by kernel writes.** boot.S:136-138 only ORs in PG, cdboot.S:254-256 only PE, and trampoline.S:59-61 PG|PE. INIT clears WP, so **every AP runs with WP=0**. The BSP keeps whatever its loader left: 0 on the BIOS/multiboot paths; UEFI firmware usually sets it. With WP=0, ring-0 writes to read-only COW pages do not fault. Syscall out-parameters and signal frames therefore go straight into a frame still shared with the other process. The COW design relies on those faults and says so explicitly (idt.c:189-198). cowtest cannot detect this: its `sysinfo` call writes into a shared stack page, but the child never reads it.
- **B4. The PMM hands out frames the identity map does not cover.** `pmm_init` frees every page-aligned usable frame below `highest` (pmm.c:89-96). `paging_init` maps usable RAM above 64 MiB only as whole 2 MiB pages rounded inward (paging.c:478-491). The partial head and tail chunks stay free but unmapped. The log's arithmetic (54640 KiB free at -m 64, which implies the usable region ends at 0x3FE0000) means QEMU/SeaBIOS end low RAM 128 KiB short of the top. At -m 256 that gives [0x0FE00000, 0x0FFE0000), 480 frames. Next-fit allocation (pmm.c:116-127) eventually reaches them, and the first identity-mapped touch (user.c:59, elf.c:94, paging.c:47) faults in ring 0, which panics. The -m 64 harness default hides it. Fix: `pmm_reserve` the unmapped partial chunks, or map them with 4 KiB pages.
- **B5. The LAPIC timer calibration is off by about 2×.** timer.c:49 programs the PIT in mode 3, where the counter reloads twice per output period. lapic.c:113-128 counts 5 "reloads" as 5 periods (50 ms), but they are 2.5 periods; the measurement also starts at an arbitrary phase. `ticks_per_second` comes out at 0.4–0.5× the real rate, so APs are preempted at roughly 200–250 Hz rather than 100 Hz, and `/sys/cpu` "apictimer" under-reports. The 2·10^8-iteration guard of port reads can also stall boot for minutes on a machine whose 8254 is gated.
- **B6. The LAPIC spurious vector 0xFF has no IDT gate.** SVR is set to 0xFF (lapic.c:48, 61), but idt_init installs gates only for 0–47, 0x80 and 0xF0–0xF2. A delivered spurious interrupt raises #NP (vector 11), which kills the interrupted program or panics. The comment at lapic.c:44-47 claims it is ignored.
- **B7. `power_off` can hang when called from a syscall.** `sys_power` runs with IF=0 (syscall.c:463-475). If SCI_EN is clear (legacy-mode firmware), or the firmware ignores the S5 write, `power_off` calls `sleep_ms` (power.c:159, 172), which `hlt`s with interrupts disabled forever instead of returning false. The terminal's shutdown button uses this path (userland/term.c:467).
- **B8. `kmalloc` truncates sizes to 32 bits.** `n = (n + 7) & ~7u` (heap.c:41): `~7u` is a 32-bit value, so any size of 4 GiB−7 or more masks to its low 32 bits, possibly 0, and a small block is returned. `kcalloc` then `memset`s the full original n (heap.c:55). This is latent unless some caller passes an unchecked large size.
- **B9. The heap is not safe against same-CPU preemption.** Kernel tasks run with IF=1 and are round-robined on every PIT tick, so a tick inside `kmalloc`/`kfree` can switch to another kernel task that uses the heap. heap.c and pmm.c have no cli, no lock and no reentrancy guard; the same applies to the task ring in `task_create` versus reaping inside the tick. The "kernel is non-preemptive" claims (sched.h:180-183, README:603-604) cover only IF=0 paths. This is a rare-corruption risk.
- **B10. A boot-time heap race.** `start_cpu` returns as soon as the AP sets `started` (smp.c:214, 279). The AP then takes the kernel lock and `kcalloc`s its idle record (sched.c:563), but the BSP does not hold the lock until `sched_start` (sched.c:600). So the BSP's next `kmalloc` (the next AP's stack, or later driver initialisation) can run concurrently with it. The window is small.
- **B11. The work API's completion semantics are weak.** `me->fn = 0` happens before `fn(a)` runs (smp.c:304), and `fn` runs with IF=1 (smp.c:302), contrary to smp.h:75-76. `smp_wait` therefore returns when a job is picked up, and the AP's idle task can be switched out in the middle of a job. `pick_next` protects only jobs still pending (sched.c:439). The selftest's "they all finished" and "the count is exact" checks (selftest.c:1266-1272) can race with an AP still incrementing. In fb.c, a stale helper that finishes late can set `helper_done` during a later frame.
- **B12. A helper that is running a program stalls frames.** `smp_helper` (smp.c:327-332) does not check that the AP is idle. `VEC_AP_WAKE` just returns into the running program (idt.c:164). While the BSP holds the lock inside `fb_flush`, which is a kernel task, the AP's local-timer try-lock always misses (idt.c:179-184), so the AP never reschedules. `fb_flush` then spins up to 20,000,000 `pause` iterations (fb.c:462) before doing the half itself. That is 0.1–1 s or more per frame on real CPUs.
- **B13. The jitter-quality gate does not do what its comment says.** `spread |= delta ^ (delta >> 17)` (rng.c:121) is non-zero for any constant non-zero interval. `timing_ok` (rng.c:127-129) therefore passes after 128 identical ticks, despite the comment at rng.c:125-126. `rng_ready()` means "the TSC runs", not "the intervals varied".
- **B14. `fpu_blank` marks all x87 registers valid.** It sets the FXSAVE abridged tag byte to 0xFF (fpu.c:98-99). In FXSAVE format a 1 bit means valid, so this is "all eight registers full", not "empty", and a fresh task's first x87 push would overflow the stack (masked, so it produces a NaN). No shipped program uses x87 (grep), so the effect is latent. The MXCSR_MASK comment (fpu.c:105-109) is also misleading: FXRSTOR ignores that field.
- **B15. `copy_table` leaks on out-of-memory.** When a nested copy fails, the freshly allocated table is never linked into `dst` (paging.c:262-267). The caller's cleanup cannot find it, and the share counts already taken for pages inside it are never released.
- **B16. Heap-backed window surfaces are treated as PMM frames.** `free_table` frees every `PTE_USER` leaf, and `copy_table` COW-shares every `PTE_USER` leaf (paging.c:203-216, 272-282). Window surfaces are kernel-heap pages mapped `PTE_USER` (winsrv.c:318-319). exec frees the old directory without `winsrv_release` (syscall.c:300), and fork COW-shares the surface. Either way a heap page can reach `mark_free` while the heap still owns it. No shipped windowed program forks or execs today.
- **B17. `test_user_access` pollutes the shared kernel tables.** It maps 0x38000000, 0x39000000 and 0x39001000 in a scratch directory that shares PML4[0] (selftest.c:212-236), which edits the shared PDPT/PD and widens USER bits. On a VM with roughly 912 MiB or more, it would split a kernel 2 MiB page and permanently repoint three identity pages at frames it then frees (`kp`, `kp2`) or leaks (`up`).
- **B18.** Negative numbers with space padding print as "-   5" (printf.c:77-84).
- **B19.** The RTC weekday is left as the CMOS 1..7 value (rtc.c:66, 133). The header says 0 = Sunday (rtc.h:23). Nothing reads it.
- **B20.** `pci_write16` does a 32-bit read-modify-write (pci.c:125-130), so `pci_enable_bus_master` writes the status word back, which clears RW1C bits. `pci_list_class`/`pci_find_class` ignore the multifunction bit (pci.c:174-215).
- **B21.** With ECAM, the first full scan (for example a failed `pci_find`) maps every bus up to the MCFG end: 256 × 1 MiB of 4 KiB UC PTEs on q35 (pci.c:72-76, 137-139). This defeats the lazy-mapping rationale at pci.c:25-27.
- **B22.** The trampoline never clears CR0.CD/NW and never sets NE/WP (trampoline.S:59-61). INIT leaves CD/NW unchanged, so APs depend on firmware having already enabled caching on them.
- **B23.** If an AP reports in after `start_cpu` gave up (about 400 ms), it runs on a trampoline already re-patched for the next CPU and shares that CPU's stack. A failed start leaks 32 KiB (smp.c:170-220).
- **B24.** Page 0 is identity mapped RW (paging.c:463), so kernel NULL dereferences read and write the real-mode IVT instead of faulting.
- **B25.** `paging_map_device` identity-maps at the physical address. A BAR at 512 GiB or above lands in PML4[1], the user half (paging.c:567-575 with paging.h:62).
- **B26.** A second SIPI is sent only after the first attempt times out (smp.c:207-217). The comment says it is sent regardless (smp.c:204-206). The SIPI also omits `ICR_ASSERT` (smp.c:209), although the comment at smp.c:351-353 insists every IPI except INIT de-assert needs it. That is harmless on modern CPUs, which ignore the bit.
- **B27. PIT ticks are lost under lock contention.** Vector 32 blocks in `kernel_lock_acquire` with IF=0 whenever an AP holds the lock (idt.c:186). A long syscall on an AP, such as RSA inside TLS in the kernel, delays or loses BSP ticks, which is the only time base, so sleeps and timeouts drift.

### 10.2 Stale or inaccurate documentation

- **D1.** README:450-453 says "Two-level paging. The low 16 MiB is identity mapped". The code has four levels, a 4 KiB low 64 MiB, 2 MiB pages up to 64 GiB, and user space at 512 GiB. README:1638-1645 and paging.c:1 are correct.
- **D2.** README:455 and heap.c:1 say "boundary tags". The heap uses a prev/next list and has no tags.
- **D3.** README:1423-1426 says the memset recursion fix was volatile pointers and that `-fno-builtin` does not help. string.c:7-14 says the volatile was removed, and string.c:3-6 still describes volatile. The build relies on `-ffreestanding -fno-builtin`.
- **D4.** README:1428-1432 says the fix was `-mcpu=i686` and that tools/check_sse.py "now fails the build". The build is x86_64 with `-mno-sse…` flags, and neither build.sh nor gate.sh calls check_sse.py.
- **D5.** README:1657 says "49 interrupt stubs", and isr.S:367-368 says "forty-nine symbols". There are 52.
- **D6.** README:1333-1359 says 552 checks and "[the identity map] 6". The log shows 556, with 2 identity-map checks at -m 64.
- **D7.** smp.h:75-76 says "The function runs with interrupts off". smp.c:302 enables interrupts first.
- **D8.** smp.c:14-20 still describes APs that only wait for handed functions while "The boot processor still owns the kernel".
- **D9.** idt.h:26-28 says `VEC_AP_WAKE` is "below the system call gate". 0xF0 is above 0x80.
- **D10.** The comment above `isr242` (isr.S:336-340) describes the yield vector, 241.
- **D11.** lapic.c:44-47 says a spurious interrupt "is ignored if it ever fires" (see B6).
- **D12.** lapic.c:108-109 says "Five reloads of the 8254 … fifty milliseconds" (see B5).
- **D13.** ioapic.c:147-150 says the scheduler does not yet run on more than one processor. It does.
- **D14.** gdt.c:53-56 says the TSSs are "each aligned". Only the array is 64-byte aligned; entries are 104 bytes apart.
- **D15.** acpi.c:179-181 says "what a 32 bit kernel can reach".
- **D16.** printf.c:57-69 says "the kernel's own half starts at 0xFFFF800000000000". zelr has no higher half.
- **D17.** divide.c:1-2 gives the 32-bit rationale; the helpers are unreferenced.
- **D18.** paging.c:56-58 says "Nothing here creates" large pages. `map_huge_in` does.
- **D19.** paging.c:241-243 and paging.h:101-104 promise COW statistics in /sys/memory. `paging_cow_saved`, `paging_cow_copies` and `pmm_shared_frames` are never reported (sysfs.c:96-106).
- **D20.** pmm.c:68 says "the heap is at a fixed address". It follows the image (main.c:67-70).
- **D21.** fpu.h:40 says "True once fpu_init has run here". It is one global flag.
- **D22.** serial.c:18 says "RTS/DSR set". 0x0B is DTR|RTS|OUT2.
- **D23.** smp.c:134-136 says `delay_us` "reads the PIT's counter". It polls port 0x61 bit 4.
- **D24.** linker.ld says the 15 MiB below the kernel "now hold nothing". Those frames are the PMM's first allocations, including the kernel page tables.
- **D25.** selftest.c:218-219 says "above where user space begins". User space starts at 512 GiB.
- **D26.** acpi.h:4 says "Just enough ACPI to find the other processors, and where PCIe lives". It also parses IOAPICs, overrides and the FADT.
- **D27.** The header comments of smp.h and sched.h describe a lock held "whenever it is not executing ring 3 code". It is not held by idle tasks or by handed functions, and B2 is the opposite violation.

---

## 11. Open questions

1. Does ring3check (256 MiB, about 20 programs) or a long desktop session cumulatively allocate enough frames to hit the unmapped tail from B4? A panic "unhandled exception 14" at a cr2 near the top of low RAM would confirm it.
2. What CR0.WP value does OVMF leave on the BSP under the UEFI loader? This decides whether B3 affects the BSP when booted through UEFI. APs are affected either way.
3. What does `/sys/cpu` "apictimer" report under `-smp 2`, compared with the expected bus clock / 16? That would confirm B5, and whether APs tick at about 250 Hz.
4. Is `smp_wait` meant to mean pickup or completion? fb.c assumes pickup, while the selftest reads it as completion.
5. Should the BSP arm its own LAPIC timer, so that timekeeping (PIT) and preemption are separated everywhere?
6. Real-hardware gaps: x2APIC-mode firmware, MADT type 9 entries, >16 CPUs, hardware-reduced ACPI power-off, PCI INTx through `_PRT`/GSI ≥ 16, and 64-bit BARs placed at 512 GiB or above.
7. Is running kernel tasks with IF=1 and relying on round-robin between them a deliberate choice (B9), or should kernel tasks keep interrupts masked except at explicit yield points?
