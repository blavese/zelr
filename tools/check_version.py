"""Checks the version the kernel reports against the newest tag.

KERNEL_VERSION is what a running machine says about itself, and what ends up
inside zelr.exe and zelr.iso. It is edited by hand and nothing has ever
depended on it, so it drifts: it sat at 0.1.0 while the tags reached 0.4.0,
and it was left at 0.10.0 while v0.11.0 was tagged and its assets uploaded,
which means both downloads announced the previous version from inside.

Being ahead of the newest tag is fine and normal: that is what the tree
looks like between bumping the version and cutting the release. Being
behind is the bug.

A tree with no tags at all, which is any fresh clone, has nothing to check
against and passes.

This reads the tags this clone has, and says nothing about the network. A
release cut with `gh release create` makes the tag on the remote, so until
`git fetch --tags` this will compare against the tag before it and report a
version that is actually behind as fine. That is how it behaved the first
time it was run, one release out of date and green.

  python tools/check_version.py
"""
import io
import os
import re
import subprocess
import sys


def kernel_version(root):
    p = os.path.join(root, "include", "types.h")
    s = io.open(p, encoding="utf-8").read()
    m = re.search(r'#define\s+KERNEL_VERSION\s+"([^"]+)"', s)
    if not m:
        print("version: no KERNEL_VERSION in include/types.h", file=sys.stderr)
        return None
    return m.group(1)


def newest_tag(root):
    try:
        out = subprocess.run(["git", "tag", "--sort=-v:refname"], cwd=root,
                             capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    for line in out.split("\n"):
        line = line.strip()
        if re.fullmatch(r"v\d+(\.\d+)*", line):
            return line
    return None


def parts(v):
    return tuple(int(n) for n in v.split("."))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    version = kernel_version(root)
    if version is None:
        return 1

    tag = newest_tag(root)
    if tag is None:
        print("      version %s, no tags to check it against" % version)
        return 0

    if parts(version) < parts(tag.lstrip("v")):
        print("version: the kernel reports %s, behind the newest tag %s. "
              "Anything built now says %s from inside."
              % (version, tag, version), file=sys.stderr)
        return 1

    print("      version %s, not behind %s" % (version, tag))
    return 0


if __name__ == "__main__":
    sys.exit(main())
