#!/usr/bin/env python3
"""Minimal FAT16 image builder with correct LFN (long file name) support.

UEFI's FatPkg resolves paths such as \\EFI\\UniXNU\\unixnu.toml case
insensitively, but it matches against the LFN entries when present and the
8.3 entry otherwise. Anything that does not fit 8.3 therefore needs real LFN
entries, otherwise the file simply cannot be opened by name.

Usage: mkfat.py <output.fat> [payload-spec.json]

Geometry is fixed: 4 MiB FAT16, 512-byte clusters (8095 clusters, legal
FAT16 range) so the image is sector-aligned at any 512-byte boundary.
"""
import json
import struct
import sys

SECTOR = 512
SPC = 1
TOTAL_SECTORS = 8192  # 4 MiB
FAT_SECTORS = 32
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
RESERVED = 1
FAT0 = RESERVED
FAT1 = FAT0 + FAT_SECTORS
ROOT = FAT1 + FAT_SECTORS
DATA = ROOT + ROOT_SECTORS
CLUSTERS = (TOTAL_SECTORS - DATA) // SPC

OEM = b'UNIXNU00'          # unique signature used by UniXNURamDiskDxe's DRAM scan
LABEL = 'UNIXNU'

DIRDATA = {}
# FAT[0] carries the media descriptor in its low byte (0xFFF8 for a fixed
# disk), FAT[1] is the end-of-chain marker; both are 16-bit little endian here.
fat = [0xFFF8, 0xFFFF] + [0] * (FAT_SECTORS * SECTOR // 2 - 2)
next_free = 2


def alloc(n):
    global next_free
    start = next_free
    if next_free + n - 1 >= CLUSTERS:
        raise SystemExit('out of clusters')
    for i in range(n):
        c = next_free
        fat[c] = c + 1 if i < n - 1 else 0xFFFF
        next_free += 1
    return start


def short_name_for(name):
    """Return (11-byte 8.3 name, needs_lfn)."""
    base, _, ext = name.upper().rpartition('.') if '.' in name else (name.upper(), '', '')
    clean = lambda s: ''.join(ch for ch in s if ch.isalnum() or ch in '_-#$%&@!(){}^~`')
    base, ext = clean(base), clean(ext)
    fits = 1 <= len(base) <= 8 and len(ext) <= 3
    if fits:
        return (base.ljust(8) + ext.ljust(3)).encode('ascii'), False
    # Generate BASE~N.EXT truncated to 8.3, guaranteed unique via next_free.
    stem = base[:6].ljust(6) + '~1'
    return (stem + ext[:3].ljust(3)).encode('ascii'), True


def lfn_checksum(sn):
    s = 0
    for ch in sn:
        s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
    return s


def lfn_entries(name, sn):
    """Long-name directory entries (must precede the 8.3 entry)."""
    ucs = name.encode('utf-16-le')
    units = len(ucs) // 2
    slots = (units + 12) // 13
    # Pad with 0x0000 terminator then 0xFFFF fillers.
    padded = ucs + b'\x00\x00'
    padded += b'\xff\xff' * (slots * 13 - units - 1)
    cksum = lfn_checksum(sn)
    out = bytearray()
    for i in range(slots, 0, -1):          # entries are stored in reverse order
        part = padded[(i - 1) * 26:i * 26]
        order = i | (0x40 if i == slots else 0)
        out += struct.pack('<B', order)
        out += part[0:10]
        out += struct.pack('<B', 0x0F)     # attr: LFN
        out += struct.pack('<B', 0)        # type
        out += struct.pack('<B', cksum)
        out += part[10:22]
        out += struct.pack('<H', 0)        # first cluster (always 0)
        out += part[22:26]
    if len(out) % 32:
        raise SystemExit('lfn entry misaligned')
    return bytes(out)


def dirent(name, attr, cluster, size):
    sn, needs_lfn = short_name_for(name) if name not in ('.', '..') else (
        (name.ljust(11)).encode('ascii'), False)
    e = bytearray()
    e += sn
    e.append(attr)
    e.append(0)                        # ntres
    e.append(0)                        # crtTimeTenth
    e += struct.pack('<HH', 0, 0x21)   # crtTime, crtDate (1980-01-01)
    e += struct.pack('<H', 0x21)       # lastAccDate
    e += struct.pack('<H', 0)          # fstClusHI
    e += struct.pack('<HH', 0, 0x21)   # wrtTime, wrtDate
    e += struct.pack('<H', cluster)    # fstClusLO
    e += struct.pack('<I', size)
    assert len(e) == 32, len(e)
    return (lfn_entries(name, sn) if needs_lfn else b'') + bytes(e)


def add_file(dirlist, name, content, attr=0x20):
    nclusters = max(1, (len(content) + SPC * SECTOR - 1) // (SPC * SECTOR))
    c = alloc(nclusters)
    DIRDATA[c] = content
    dirlist.append(dirent(name, attr, c, len(content)))
    return c


def add_dir(dirlist, name, parent_cluster, children_cb):
    c = alloc(1)
    inner = [dirent('.', 0x10, c, 0), dirent('..', 0x10, parent_cluster, 0)]
    children_cb(inner, c)
    blob = b''.join(inner)
    if len(blob) > SPC * SECTOR:
        raise SystemExit('directory too large for one cluster: %s' % name)
    DIRDATA[c] = blob
    dirlist.append(dirent(name, 0x10, c, 0))
    return c


TOML = b"""version = 1

[boot]
timeout = -1
fallback_timeout = 30
default = "adt"
rotate = 2

[ui]
title = "UniXNU"

[[entry]]
id = "adt"
title = "phoenix ADT inspector (payload volume + device tree)"
type = "efi"
path = "Payloads/ADTInspector.efi"
args = ""

[[entry]]
id = "hello"
title = "Hello payload (efi payload from FAT)"
type = "efi"
path = "Payloads/HelloPayload.efi"
args = "pause"

[[entry]]
id = "xnu"
title = "XNU kernel (payload test)"
type = "xnu"
path = "Payloads/kernel.macho"
args = "-v serial=3"

[[entry]]
id = "exit"
title = "Return to firmware"
type = "action"
action = "exit"
"""


def build(extra_files=None, toml=None):
    extra_files = extra_files or {}
    toml = TOML if toml is None else toml
    # Paths given without an "EFI/" prefix live under \EFI\UniXNU, which is
    # where the TOML paths are relative to.
    # Normalise every extra path to a volume-root-relative tuple. Bare paths
    # (no "EFI/" prefix) are placed under \EFI\UniXNU, matching how the TOML
    # references payloads.
    tree = {'dirs': {}, 'files': {}}

    def node_for(parts):
        node = tree
        for part in parts:
            node = node['dirs'].setdefault(part, {'dirs': {}, 'files': {}})
        return node

    node_for(('EFI', 'UniXNU'))['files']['unixnu.toml'] = toml

    for rel, blob in extra_files.items():
        parts = tuple(p for p in rel.replace('\\', '/').split('/') if p)
        if not parts:
            continue
        if parts[0].upper() != 'EFI':
            parts = ('EFI', 'UniXNU') + parts
        node_for(parts[:-1])['files'][parts[-1]] = blob

    def emit_dir(parent_list, parent_cluster, node):
        for fname, blob in sorted(node['files'].items()):
            add_file(parent_list, fname, blob)
        for dname, child in sorted(node['dirs'].items()):
            add_dir(parent_list, dname, parent_cluster,
                    lambda lst, cl, c=child: emit_dir(lst, cl, c))

    root = [dirent(LABEL, 0x08, 0, 0)]  # volume label entry
    emit_dir(root, 0, tree)

    img = bytearray(TOTAL_SECTORS * SECTOR)
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xeb\x3c\x90'
    bs[3:11] = OEM
    struct.pack_into('<H', bs, 11, SECTOR)      # bytes per sector
    bs[13] = SPC                                # sectors per cluster
    struct.pack_into('<H', bs, 14, RESERVED)
    bs[16] = 2                                  # number of FATs
    struct.pack_into('<H', bs, 17, ROOT_ENTRIES)
    struct.pack_into('<H', bs, 19, 0)           # total sectors 16 (0 => use 32)
    bs[21] = 0xF8                               # media descriptor: fixed disk
    struct.pack_into('<H', bs, 22, FAT_SECTORS)
    struct.pack_into('<H', bs, 24, 32)          # sectors per track
    struct.pack_into('<H', bs, 26, 8)           # heads
    struct.pack_into('<I', bs, 28, 0)           # hidden sectors
    struct.pack_into('<I', bs, 32, TOTAL_SECTORS)
    bs[36] = 0x80                               # drive number
    bs[38] = 0x29                               # extended boot signature
    struct.pack_into('<I', bs, 39, 0x20261107)  # volume serial
    bs[43:54] = LABEL.ljust(11).encode()
    bs[54:62] = b'FAT16   '
    bs[510:512] = b'\x55\xaa'
    img[0:SECTOR] = bs

    fatbytes = b''.join(struct.pack('<H', v) for v in fat)
    img[FAT0 * SECTOR:(FAT0 + FAT_SECTORS) * SECTOR] = fatbytes
    img[FAT1 * SECTOR:(FAT1 + FAT_SECTORS) * SECTOR] = fatbytes

    rootblob = b''.join(root)
    if len(rootblob) > ROOT_SECTORS * SECTOR:
        raise SystemExit('root directory overflow')
    img[ROOT * SECTOR:ROOT * SECTOR + len(rootblob)] = rootblob

    for c, content in DIRDATA.items():
        off = DATA * SECTOR + (c - 2) * SPC * SECTOR
        img[off:off + len(content)] = content

    return bytes(img)


if __name__ == '__main__':
    import argparse

    ap = argparse.ArgumentParser(description='build a FAT16 payload image')
    ap.add_argument('output')
    ap.add_argument('--toml', help='TOML file to install as \\EFI\\UniXNU\\unixnu.toml')
    ap.add_argument('--add', action='append', default=[], metavar='GOPATH=HOSTPATH',
                    help='add a file at path GOPATH (relative to the volume root)')
    args = ap.parse_args()

    toml = TOML
    if args.toml:
        with open(args.toml, 'rb') as fh:
            toml = fh.read()

    extra = {}
    for spec in args.add:
        gopath, _, hostpath = spec.partition('=')
        if not hostpath:
            raise SystemExit('bad --add %r, expected GOPATH=HOSTPATH' % spec)
        with open(hostpath, 'rb') as fh:
            extra[gopath.strip('/')] = fh.read()

    data = build(extra, toml)
    with open(args.output, 'wb') as fh:
        fh.write(data)
    print('wrote %s, %d bytes, %d clusters used' % (args.output, len(data), next_free - 2))
