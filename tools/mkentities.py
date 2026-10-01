"""Writes userland/entities.h: every named character reference HTML has,
from the Python standard library's copy of the standard's table.

    python tools/mkentities.py

The names are sorted, for html.h to halve its way to one. A name the
standard also allows without its semicolon (the old ones: amp, copy, eacute)
is marked, since only those may be read without one. The few that are two
characters keep the first: the second is a mark the text, which is folded
to ASCII (html.h, html_fold_cp), has no way to show."""
import html.entities
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
table = html.entities.html5
names = sorted(k[:-1] for k in table if k.endswith(";"))
bare = {k for k in table if not k.endswith(";")}

lines = []
for n in names:
    cps = [ord(c) for c in table[n + ";"]]
    lines.append('    { "%s", 0x%X, %d },' % (n, cps[0], 1 if n in bare else 0))

out = """/* Every named character reference HTML has, sorted by name (html.h,
   html_named). Written by tools/mkentities.py from the standard's table, as
   Python's library carries it; do not edit by hand. `bare` names the old
   ones a page may write without the semicolon. */
#pragma once

typedef struct { const char *name; unsigned cp; unsigned char bare; } html_entity;

#define HTML_ENTITIES %d

static const html_entity HTML_ENTITY[HTML_ENTITIES] = {
%s
};
""" % (len(names), "\n".join(lines))
open(os.path.join(ROOT, "userland", "entities.h"), "w", encoding="utf-8", newline="\n").write(out)
print("wrote userland/entities.h: %d names, %d without a semicolon" % (len(names), len(bare)))
