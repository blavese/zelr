"""A network system call whose peer never answers freezes the whole machine.

Found by reading (docs/atlas/06a-network.md section 10) and reproduced here on
2026-09-26 against commit 6048716. Not a gate step: this is the evidence and
the starting point for a fix, and it is expected to report FROZEN until the
fix lands.

Why it happens. Every gate in the IDT is an interrupt gate, int 0x80
included (kernel/idt.c:63), so a system call runs with interrupts off, and
`ticks` only advances in the timer interrupt (kernel/timer.c:18). The network
waits are all `while (timer_ticks() < deadline) net_poll();` loops: ARP
(kernel/net.c:162-172), TCP open, send and receive (kernel/tcp.c:426-494),
DNS, DHCP and ping. Inside a system call the deadline can never arrive.
Frames still get in, because net_poll polls the card, which is why the
ordinary path works and the gate is green.

How this shows it. QEMU's user network with restrict=on answers DHCP (with no
router) and nothing else, so a connection to a public address is never
answered. The kernel shell's own `fetch`, a kernel task with interrupts on,
is the control: it gives up after about two seconds. The same address asked
for from ring 3 stops the machine: RIP stays in ring 0 with IF clear and the
tick counter does not move again.

    python docs/atlas/repro/nethang.py        (build first: bash build.sh)
"""
import os
import re
import struct
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
os.environ["ZELR_PREBUILT"] = "1"
from harness import Guest  # noqa: E402

DEAD = "93.184.216.34"
NET = ["-netdev", "user,id=n0,restrict=on", "-device", "e1000,netdev=n0"]


def elf64_symbols(path):
    """(value, name, kind) for every named symbol, straight from .symtab."""
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2, "expected ELF64"
    e_shoff, = struct.unpack_from("<Q", d, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", d, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", d, e_shoff + i * e_shentsize)
            for i in range(e_shnum)]
    out = []
    for s in secs:
        if s[1] != 2:                                   # SHT_SYMTAB
            continue
        strtab = secs[s[6]][4]
        for j in range(s[5] // s[9]):
            name, info, _, _, value, _ = struct.unpack_from("<IBBHQQ", d, s[4] + j * s[9])
            if name and value:
                end = d.index(b"\0", strtab + name)
                out.append((value, d[strtab + name:end].decode(), info & 0xF))
    return sorted(out)


def function_at(syms, addr):
    best = None
    for value, name, kind in syms:
        if kind == 2 and value <= addr:
            best = (value, name)
    return "%s+0x%x" % (best[1], addr - best[0]) if best else "?"


def boot():
    g = Guest(os.path.join(ROOT, "build", "repro-nethang.img"), memory=256, extra=NET)
    g.wait_boot(120)
    time.sleep(5)                                       # the boot-time dhcp task
    return g


def control():
    g = boot()
    try:
        want = g.prompts() + 1
        t0 = time.time()
        g.type("fetch %s /\n" % DEAD)
        back = g.wait_prompt(want, timeout=60)
        return back, time.time() - t0
    finally:
        g.stop()


def frozen(syms, ticks_at):
    g = boot()
    try:
        mon = g.monitor()

        def ticks():
            m = re.search(r":\s*0x([0-9a-fA-F]+)", mon.send("xp /1gx 0x%x" % ticks_at))
            return int(m.group(1), 16) if m else None

        g.type("exec /bin/wiretest http://%s:80\n" % DEAD)
        time.sleep(15)
        before = ticks()
        samples = []
        for _ in range(5):
            regs = mon.send("info registers")
            rip = int(re.search(r"RIP=([0-9a-fA-F]+)", regs).group(1), 16)
            rfl = int(re.search(r"RFL=([0-9a-fA-F]+)", regs).group(1), 16)
            cpl = int(re.search(r"CPL=(\d)", regs).group(1))
            samples.append((rip, (rfl >> 9) & 1, cpl))
            time.sleep(1)
        after = ticks()
        want = g.prompts() + 1
        g.type("echo still-alive\n")
        answered = g.wait_prompt(want, timeout=20)
        return before, after, samples, answered
    finally:
        g.stop()


def main():
    syms = elf64_symbols(os.path.join(ROOT, "build", "zelr.elf"))
    ticks_at = [v for v, n, k in syms if n == "ticks" and k == 1][0]

    back, secs = control()
    print("control, kernel fetch: prompt came back=%s after %.1f s" % (back, secs))

    before, after, samples, answered = frozen(syms, ticks_at)
    for rip, iflag, cpl in samples:
        print("  RIP %016x %-28s IF=%d CPL=%d" % (rip, function_at(syms, rip), iflag, cpl))
    print("ticks %s -> %s over ~5 s; shell answered afterwards: %s" % (before, after, answered))

    is_frozen = before == after and not answered and all(i == 0 and c == 0 for _, i, c in samples)
    print("FROZEN" if is_frozen else "NOT FROZEN")
    return 1 if is_frozen else 0


if __name__ == "__main__":
    sys.exit(main())
