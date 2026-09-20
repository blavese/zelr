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


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def kernel_defaults():
    """What theme_init sets, which is what a machine with no file gets."""
    src = read(THEME)
    body = src[src.index("void theme_init(void)"):]
    body = body[:body.index("\n}")]

    out = {}
    for name, key in (("wallpaper", "wallpaper"), ("corner", "corner"),
                      ("shadows", "shadows"), ("animate", "animate"),
                      ("quirks", "quirks"), ("autodesktop", "autodesktop"),
                      ("volume", "volume")):
        m = re.search(r"current\.%s\s*=\s*([A-Za-z0-9_]+)\s*;" % name, body)
        if m:
            v = m.group(1)
            out[key] = {"true": "1", "false": "0"}.get(v, v)

    m = re.search(r"current\.look\s*=\s*(LOOK_[A-Z]+)\s*;", body)
    if m:
        out["look"] = "0" if m.group(1) == "LOOK_MODERN" else "1"
    m = re.search(r"current\.light\s*=\s*(true|false)\s*;", body)
    if m:
        out["light"] = "1" if m.group(1) == "true" else "0"
    m = re.search(r"theme_apply_preset\((\d+)\)", body)
    if m:
        out["preset"] = m.group(1)

    # The wallpaper is named rather than numbered, so it has to be looked up
    # in the enum the name comes from.
    if out.get("wallpaper", "").startswith("WALLPAPER_"):
        names = enum_order()
        want = out["wallpaper"]
        out["wallpaper"] = str(names.index(want)) if want in names else "?"
    return out


def enum_order():
    """The wallpaper enum, in order, which is what the numbers in the file
    mean. Settings stores an index into this and so does the theme."""
    src = read(os.path.join(ROOT, "include", "theme.h"))
    body = src[src.index("} wallpaper_t;") - 800:src.index("} wallpaper_t;")]
    return re.findall(r"(WALLPAPER_[A-Z]+)\s*[,=]", body)


def settings_defaults():
    """The initialisers at the top of settings.c."""
    src = read(SETTINGS)
    out = {}
    for key in ("preset", "light", "look", "wallpaper", "corner", "shadows",
                "animate", "quirks", "autodesktop"):
        m = re.search(r"^static int %s\s*=\s*(-?\d+)\s*;" % key, src, re.M)
        if m:
            out[key] = m.group(1)
    return out


def settings_saved_keys():
    """Every key the settings program writes. A key the kernel reads and this
    never writes is a key that disappears the first time anything is saved."""
    src = read(SETTINGS)
    body = src[src.index("static void save(void)"):]
    body = body[:body.index("\n}")]
    return set(re.findall(r'put_kv\(out, n, "([a-z_]+)"', body))


def kernel_read_keys():
    """And every key the kernel will read back out of the file."""
    src = read(THEME)
    return set(re.findall(r'strcmp\(key, "([a-z_]+)"\)', src))


def main():
    c = Checks("the defaults")

    k = kernel_defaults()
    s = settings_defaults()

    c.add("the wallpaper enum could be read", len(enum_order()) > 4)

    for key in sorted(set(k) & set(s)):
        c.add("%s: the kernel says %s and settings says %s"
              % (key, k[key], s[key]), k[key] == s[key])

    missing = sorted(set(s) - set(k))
    c.add("settings has no default the kernel does not"
          + (" (%s)" % ", ".join(missing) if missing else ""), not missing)

    # A key the kernel reads, that settings holds a value for, and that
    # settings does not write, is silently reset on every save.
    written = settings_saved_keys()
    read_back = kernel_read_keys()
    dropped = sorted((read_back & set(s)) - written)
    c.add("every setting it holds is a setting it writes"
          + (" (drops %s)" % ", ".join(dropped) if dropped else ""),
          not dropped)

    return c.report()


if __name__ == "__main__":
    sys.exit(main())
