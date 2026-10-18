#!/usr/bin/env python3
"""Fail the build if an SSE or MMX opcode reached an executable section.

A kernel that has not enabled the FPU takes #UD on the first one, and the
fault lands somewhere far from the cause, so this is worth catching at build
time. That happened once already: zig's default x86 target has SSE2 on, and
clang used `movd xmm0` for a 64-bit integer move.

This is a byte scanner, not a disassembler. It does not know where
instructions begin, so it can match bytes that are really part of a preceding
instruction's immediate operand. String addresses pushed before a call are the
common case: `push $0x00110f00` contains the bytes 0f 11 00, which look like
`movups`. Those are filtered by checking whether the surrounding bytes read as
an address inside the image; anything that does is an operand, not an opcode.

The operand-size prefixed forms (66 0F ..., the SSE2 moves) are reported
unless they sit in a displacement relative to rip that reaches into the
image: no immediate spells that prefix by accident, but a global's distance
can. 0.112.0's kernel had `mov [rip+0x00d60f66], rax`, 48 89 05 66 0f d6 00,
once the programs pasted into it grew.
"""
import struct
import sys

# 66 0F xx: operand-size prefixed SSE2. Unambiguous.
PREFIXED = {0x6E, 0x7E, 0xD6, 0x6F, 0x7F, 0x28, 0x29}
# 0F xx on its own: real SSE, but also a common byte pair inside immediates.
BARE = {0x10, 0x11, 0x28, 0x29, 0x58, 0x59, 0x6E, 0x7E}


def sections(data):
    """Both ELF widths. The section header is not the 32-bit one widened:
    the fields are in the same order but the addresses and offsets grow, so
    the layouts have to be spelled out separately."""
    wide = data[4] == 2

    if wide:
        e_shoff, = struct.unpack_from("<Q", data, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    else:
        e_shoff, = struct.unpack_from("<I", data, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)

    out = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        if wide:
            name, typ, flags, addr, off, size = struct.unpack_from("<IIQQQQ", data, o)
        else:
            name, typ, flags, addr, off, size = struct.unpack_from("<IIIIII", data, o)
        out.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size))
    shstr = out[e_shstrndx]
    for s in out:
        raw = data[shstr["off"] + s["name"]:]
        s["label"] = raw[:raw.index(b"\0")].decode(errors="replace")
    return out


def image_range(secs):
    lo = min((s["addr"] for s in secs if s["addr"]), default=0)
    hi = max((s["addr"] + s["size"] for s in secs if s["addr"]), default=0)
    return lo, hi


def in_rip_displacement(body, j, s, lo, hi):
    """Whether the bytes at j sit in the four byte displacement of an
    instruction that reaches memory relative to rip, one whose target is in
    the image. On x86-64 nearly every access to a global is one of those.
    The giveaway is the addressing byte in front: mod 00 with rm 101 is the
    encoding that means "relative to rip"."""
    import struct
    for back in (0, 1, 2, 3):
        k = j - back
        if k < 1 or k + 4 > len(body):
            continue
        if (body[k - 1] & 0xC7) != 0x05:
            continue
        disp, = struct.unpack_from("<i", body, k)
        target = s["addr"] + k + 4 + disp
        if lo <= target < hi:
            return True
    return False


def main():
    data = open(sys.argv[1], "rb").read()
    secs = sections(data)
    lo, hi = image_range(secs)

    hard, filtered = [], 0

    for s in secs:
        if s["type"] != 1 or not (s["flags"] & 0x4):
            continue
        body = data[s["off"]:s["off"] + s["size"]]

        for j in range(len(body) - 2):
            prefixed = body[j] == 0x66 and body[j + 1] == 0x0F and body[j + 2] in PREFIXED
            bare = body[j] == 0x0F and body[j + 1] in BARE
            if not prefixed and not bare:
                continue

            if prefixed and in_rip_displacement(body, j, s, lo, hi):
                filtered += 1
                continue

            if bare and not prefixed:
                # The first byte of a move-to-register's immediate, whatever
                # it holds: `mov esi, 0x100f` is be 0f 10 00 00, and 0x100f
                # is no address, so the test below never excused it.
                if j > 0 and 0xB8 <= body[j - 1] <= 0xBF:
                    filtered += 1
                    continue

                # Anywhere in a 64-bit immediate: `movabs rax, imm64` is a REX
                # prefix with W set, B8 to BF, and eight bytes of value. 0.116.0's
                # self test filled a key block with 1, 2, ... 40, which clang
                # wrote as such moves, and 48 b8 09 0a 0b 0c 0d 0e 0f 10 holds
                # 0f 10 in its seventh and eighth.
                if any(j - back >= 0 and 0x48 <= body[j - back] <= 0x4F and
                       0xB8 <= body[j - back + 1] <= 0xBF
                       for back in range(2, 10)):
                    filtered += 1
                    continue

                # A scale-index byte. The addressing byte in front says one
                # follows (rm 100, with mod 01 or 10), and 0F is an ordinary
                # value for it: `movzx ecx, word [rdi+rcx+0x10]` is
                # 0f b7 4c 0f 10, with the second 0F as the index byte.
                if j > 0 and (body[j - 1] & 0xC7) in (0x44, 0x84):
                    filtered += 1
                    continue

                # The first byte of a register-plus-address displacement: an
                # addressing byte with mod 10 and no scale-index byte (rm not
                # 100), then four bytes that are an address in the image. The
                # kernel's statics are at their own addresses, so an array is
                # reached so: 0.156.0's nvme_init had `movzx eax, byte
                # [rbx+0x016b100f]`, 0f b6 83 0f 10 6b 01, once the image grew.
                if (j > 0 and (body[j - 1] & 0xC0) == 0x80 and (body[j - 1] & 7) != 4
                        and j + 4 <= len(body)):
                    value, = struct.unpack_from("<I", body, j)
                    if lo <= value < hi:
                        filtered += 1
                        continue

                # Does this sit inside a 32-bit immediate holding an address?
                # First handle an address at the start of a known immediate.
                # This is common for `push $string`, where the first two bytes
                # of the address can otherwise look exactly like an opcode.
                looks_like_operand = False
                address_immediate = (
                    j > 0 and
                    (body[j - 1] == 0x68 or
                     0xB8 <= body[j - 1] <= 0xBF or
                     body[j - 1] in (0xA1, 0xA3))
                )
                if address_immediate and j + 4 <= len(body):
                    value, = struct.unpack_from("<I", body, j)
                    looks_like_operand = lo <= value < hi

                # In a displacement relative to rip: the common case on
                # x86-64 rather than a corner.
                if in_rip_displacement(body, j, s, lo, hi):
                    looks_like_operand = True
                if looks_like_operand:
                    filtered += 1
                    continue

                # A relative call or jump carries a signed displacement
                # rather than an address, so the bytes never look like one.
                # Resolve it instead: a displacement that lands inside this
                # section is a displacement, not an instruction.
                for back in (0, 1, 2, 3):
                    k = j - back
                    if k < 1 or k + 4 > len(body):
                        continue
                    if body[k - 1] not in (0xE8, 0xE9):
                        continue
                    disp, = struct.unpack_from("<i", body, k)
                    target = s["addr"] + k + 4 + disp
                    if s["addr"] <= target < s["addr"] + s["size"]:
                        looks_like_operand = True
                        break

                # Also try alignments that place the bytes mid-operand.
                for back in (1, 2, 3):
                    k = j - back
                    if k < 0 or k + 4 > len(body):
                        continue
                    value, = struct.unpack_from("<I", body, k)
                    if lo <= value < hi:
                        looks_like_operand = True
                        break
                if looks_like_operand:
                    filtered += 1
                    continue

            hard.append((s["addr"] + j, s["label"], body[j:j + 6].hex()))

    print(f"SSE/MMX opcodes in executable sections: {len(hard)}"
          f"   ({filtered} byte matches filtered as operands)")
    for addr, label, raw in hard[:10]:
        print(f"  {addr:#010x} in {label}: {raw}")

    return 1 if hard else 0


if __name__ == "__main__":
    sys.exit(main())
