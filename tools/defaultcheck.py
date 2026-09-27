"""The kernel and the settings program, on what a new machine looks like.

Two programs hold the defaults for the desktop. kernel/theme.c has them
because it has to draw something before anybody has written a settings file;
userland/settings.c has them because it has to show something before it has
read one. Nothing makes them agree.

When they disagree the symptom is peculiar enough to waste an afternoon on.
The desktop comes up one way, Settings opens reporting another, and the
moment anything at all is changed in that window the whole file is written
out of Settings' idea of the world and the desktop changes under you. The
comment at the top of settings.c is about that happening once already.

It happened again the day the modern look went in: the kernel gained a `look`
key and Settings did not, so saving anything deleted the key and left a
machine half in one look and half in the other. That is what this is for.

Most settings no longer exist twice: they are KNOBS[] in theme.c and reach
Settings through /sys/settings with their defaults. What is still written in
both places is the palette -- look, light and preset -- and what is still
keyed by hand is the colours. This checks those, and checks that it found
them: it used to look for knob defaults Settings no longer keeps and for a
function save() no longer calls, found nothing on either side, and passed
three checks that compared nothing at all.

Reading the source rather than running anything, because this is a question
about two files and not about a machine.

  python tools/defaultcheck.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Checks, ROOT                                # noqa: E402

THEME = os.path.join(ROOT, "kernel", "theme.c")
SETTINGS = os.path.join(ROOT, "userland", "settings.c")

PALETTE = ("look", "light", "preset")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def body_of(src, head):
    body = src[src.index(head):]
    return body[:body.index("\n}")]


def kernel_defaults():
    """What theme_init sets for the palette, which is what a machine with no
    file gets."""
    body = body_of(read(THEME), "void theme_init(void)")
    out = {}
    m = re.search(r"current\.look\s*=\s*(LOOK_[A-Z]+)\s*;", body)
    if m:
        out["look"] = "0" if m.group(1) == "LOOK_MODERN" else "1"
    m = re.search(r"current\.light\s*=\s*(true|false)\s*;", body)
    if m:
        out["light"] = "1" if m.group(1) == "true" else "0"
    m = re.search(r"theme_apply_preset\((\d+)\)", body)
    if m:
        out["preset"] = m.group(1)
    return out


def settings_defaults():
    """The initialisers at file scope in settings.c, several to a line as
    they are written there: `static int light = 1, look = 0, preset = 1`."""
    out = {}
    for decl in re.findall(r"^static int ([^;()]+);", read(SETTINGS), re.M):
        for part in decl.split(","):
            m = re.match(r"\s*([a-z_]+)\s*=\s*(-?\d+)\s*$", part)
            if m and m.group(1) in PALETTE:
                out[m.group(1)] = m.group(2)
    return out


def knob_keys():
    """The keys KNOBS[] describes. Settings writes every one of them from
    what /sys/settings lists, so they cannot go missing on a save."""
    src = read(THEME)
    table = src[src.index("KNOBS[]"):]
    table = table[:table.index("};")]
    return set(re.findall(r'\{\s*"([a-z_]+)"', table))


def kernel_read_keys():
    """Every other key the kernel reads back out of the file."""
    return set(re.findall(r'strcmp\(key, "([a-z_]+)"\)', read(THEME))) - knob_keys()


def settings_saved_keys():
    """Every key save() writes by name."""
    body = body_of(read(SETTINGS), "static void save(void)")
    return set(re.findall(r'line_(?:num|hex)\(out, n, "([a-z_]+)"', body))


def settings_keeps_the_rest():
    """Whether save() carries over the lines it does not write itself, which
    is what keeps a key the kernel reads and Settings has no control for."""
    return "keep_unmanaged(" in body_of(read(SETTINGS), "static void save(void)")


def main():
    c = Checks("the defaults")

    k = kernel_defaults()
    s = settings_defaults()
    c.add("the palette defaults were found on both sides",
          set(k) == set(PALETTE) and set(s) == set(PALETTE))
    for key in PALETTE:
        if key in k and key in s:
            c.add("%s: the kernel says %s and settings says %s"
                  % (key, k[key], s[key]), k[key] == s[key])

    # A key the kernel reads and Settings neither writes nor carries over is
    # deleted from the machine the first time anything is saved.
    read_back = kernel_read_keys()
    written = settings_saved_keys()
    c.add("the keys were found on both sides (%d read, %d written)"
          % (len(read_back), len(written)),
          len(read_back) >= 5 and len(written) >= 5)
    dropped = sorted(read_back - written)
    kept = settings_keeps_the_rest()
    c.add("no key the kernel reads is lost when settings saves"
          + (" (drops %s)" % ", ".join(dropped) if dropped and not kept else ""),
          not dropped or kept)

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
