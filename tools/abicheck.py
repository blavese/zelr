"""Compares the two sides of the system call interface.

Everything here is declared twice: once in include/syscall.h, which the
kernel fills in, and once in userland/zelr.h, which a program hands to the
syscall. Nothing makes the two agree.

For structs, the kernel copies sizeof(its own struct) into the address the
program gave it, so a field that is wider on the kernel's side is written
straight past the end of the program's variable and into whatever the
compiler put after it. That has now happened twice for the same reason, a
name that grew:

  - readdir wrote 64 bytes into a 32 byte name, over the register frame the
    syscall returns through, and `ls /` came back empty in ring 3
  - tasks did the same thing over the locals of whatever called it, and the
    system monitor read every task as using none of the processor

For the numbers, the failure is quieter still: a program asking for call 45
and getting call 46 does not crash, it does something else, and which
something depends on what the registers happened to hold. Nothing in the
build notices, because each header compiles perfectly on its own.

Both kinds were found by looking at a screen and wondering why it was
wrong. This reads both headers instead, and takes a minute of nobody's time.

  python tools/abicheck.py
"""
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Checks, ROOT                         # noqa: E402

KERNEL = os.path.join(ROOT, "include", "syscall.h")
USER = os.path.join(ROOT, "userland", "zelr.h")

# The kernel's name for each shared struct, and the program's name for it.
PAIRS = [
    ("zelr_task_t", "zelr_task"),
    ("zelr_stat_t", "zelr_stat"),
    ("zelr_sysinfo_t", "zelr_sysinfo"),
    ("zelr_netinfo_t", "zelr_netinfo"),
    ("sound_info_t", "zelr_sound"),
]

# What the two sides call the same width. The kernel header has the kernel's
# types; a program only ever sees the small ones.
SAME = {"unsigned int": "u32", "unsigned long long": "u64", "int": "i32"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def body_of(text, name):
    """The braces of `typedef struct { ... } name;`."""
    m = re.search(r"typedef\s+struct\s*\{([^{}]*)\}\s*" + re.escape(name)
                  + r"\s*;", text, flags=re.S)
    return m.group(1) if m else None


def fields(body):
    """[(type, name, elements)], in order, with `u32 a, b;` split up."""
    out = []
    for decl in body.split(";"):
        decl = " ".join(decl.split())
        if not decl:
            continue
        # The type is everything up to the first declarator.
        m = re.match(r"([A-Za-z_][A-Za-z0-9_ ]*?)\s+([A-Za-z_].*)$", decl)
        if not m:
            return None
        ctype, rest = m.group(1).strip(), m.group(2)
        ctype = SAME.get(ctype, ctype)
        for one in rest.split(","):
            one = one.strip()
            arr = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*(\d+)\s*\]$", one)
            if arr:
                out.append((ctype, arr.group(1), int(arr.group(2))))
            elif re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", one):
                out.append((ctype, one, 1))
            else:
                return None
    return out


def describe(f):
    return "%s %s%s" % (f[0], f[1], "[%d]" % f[2] if f[2] != 1 else "")


def numbers(text):
    """{SYS_NAME: number} for every call the header gives a number to."""
    out = {}
    for name, value in re.findall(
            r"#define\s+(SYS_[A-Z0-9_]+)\s+(\d+)", text):
        out[name] = int(value)
    return out


def check_numbers(c, ktext, utext):
    kn, un = numbers(ktext), numbers(utext)

    # Both sides having the same names matters as much as the same values: a
    # call the kernel serves and no program can name is dead, and one a
    # program names and the kernel does not serve returns -1 forever.
    only_kernel = sorted(set(kn) - set(un))
    only_user = sorted(set(un) - set(kn))
    c.add("every call the kernel serves has a name programs can use",
          not only_kernel)
    if only_kernel:
        print("  only in include/syscall.h: %s" % ", ".join(only_kernel))
    c.add("and every name programs use is one the kernel serves",
          not only_user)
    if only_user:
        print("  only in userland/zelr.h: %s" % ", ".join(only_user))

    disagree = sorted(n for n in set(kn) & set(un) if kn[n] != un[n])
    for n in disagree:
        print("  %s is %d to the kernel and %d to a program"
              % (n, kn[n], un[n]))
    c.add("the numbers agree on both sides", not disagree)

    # Two calls sharing a number means the table has one of them in it and
    # the other silently runs the wrong handler.
    seen = {}
    clash = []
    for name, v in sorted(kn.items()):
        if v in seen:
            clash.append("%d is both %s and %s" % (v, seen[v], name))
        seen[v] = name
    for line in clash:
        print("  %s" % line)
    c.add("no two calls share a number", not clash)


def main():
    ktext = strip_comments(io.open(KERNEL, encoding="utf-8").read())
    utext = strip_comments(io.open(USER, encoding="utf-8").read())
    c = Checks("the two sides of the syscall")

    check_numbers(c, ktext, utext)

    for kname, uname in PAIRS:
        kbody, ubody = body_of(ktext, kname), body_of(utext, uname)
        if kbody is None or ubody is None:
            c.add("%s is declared on both sides" % kname, False)
            continue

        kf, uf = fields(kbody), fields(ubody)
        if kf is None or uf is None:
            c.add("%s reads as a list of fields" % kname, False)
            continue

        same = len(kf) == len(uf) and all(
            a[0] == b[0] and a[2] == b[2] for a, b in zip(kf, uf))

        if not same:
            print("  %s and %s differ:" % (kname, uname))
            for i in range(max(len(kf), len(uf))):
                a = describe(kf[i]) if i < len(kf) else "-"
                b = describe(uf[i]) if i < len(uf) else "-"
                mark = " " if a == b else "*"
                print("    %s %-28s %s" % (mark, a, b))

        c.add("%s is the same shape as %s" % (kname, uname), same)

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
