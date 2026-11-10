#!/usr/bin/env python3
"""find-bst.py: print an OpenCore rename patch (_BST -> XBST) for each battery
_BST method in a DSDT, for SSDT-BATFULL (docs/BATTERY-FULL.md).

    python3 tools/find-bst.py DSDT.aml

For every Method (_BST) it prints a Find/Replace pair that is unique in the
table (the method header plus as many body bytes as needed). Pick the one of
the battery you adapt SSDT-BATFULL for: if there are several (BAT0, BAT1),
match them by their order in the disassembled DSDT (iasl -d DSDT.aml).
"""
import base64
import sys


def pkg_len(data, i):
    """Decode an AML PkgLength at i; return (value, size) or None."""
    lead = data[i]
    extra = lead >> 6
    if extra == 0:
        return lead & 0x3F, 1
    if i + extra >= len(data) or lead & 0x30:
        return None
    value = lead & 0x0F
    for k in range(extra):
        value |= data[i + 1 + k] << (4 + 8 * k)
    return value, 1 + extra


def methods(data):
    pos = 0
    while True:
        n = data.find(b"_BST", pos)
        if n < 0:
            return
        pos = n + 1
        for size in range(1, 5):
            start = n - size - 1
            if start < 0 or data[start] != 0x14:
                continue
            pl = pkg_len(data, start + 1)
            if pl and pl[1] == size and start + 1 + pl[0] <= len(data):
                yield start, start + 1 + pl[0]
                break


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"DSDT":
        print("warning: not a DSDT (signature %r)" % data[:4])
    found = 0
    for start, end in methods(data):
        found += 1
        name = data.find(b"_BST", start)
        length = name - start + 5
        while data.count(data[start:start + length]) > 1 and start + length < end:
            length += 1
        find = data[start:start + length]
        unique = data.count(find) == 1
        replace = find.replace(b"_BST", b"XBST", 1)
        print("Method _BST #%d at 0x%x (%d bytes)%s" % (found, start, end - start,
              "" if unique else "  NOT UNIQUE: use Skip/Count or a longer Find"))
        print("  Find     %s  (%s)" % (find.hex().upper(), base64.b64encode(find).decode()))
        print("  Replace  %s  (%s)" % (replace.hex().upper(), base64.b64encode(replace).decode()))
        print("  OpenCore: ACPI > Patch, TableSignature DSDT (RFNEVA==), Count 1")
    if not found:
        print("no Method (_BST) found: the battery may be defined in an SSDT")


if __name__ == "__main__":
    main()
