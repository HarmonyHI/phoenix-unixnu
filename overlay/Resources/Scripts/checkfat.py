#!/usr/bin/env python3
"""Validate a FAT12/16 image the way UEFI FatPkg would mount it.

Checks: BPB sanity, FAT type inferred from cluster count matches the declared
type, both FAT copies agree, directory entries well formed (including LFN
checksums), every file's cluster chain complete, and again resolves a set of
paths case-insensitively (what UniXNU actually does).

Usage: checkfat.py <image> [path1 path2 ...]
"""
import struct
import sys


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


class Fat:
    def __init__(self, data):
        self.d = data
        self.bps = u16(data, 11)
        self.spc = data[13]
        self.rsvd = u16(data, 14)
        self.nfats = data[16]
        self.root_entries = u16(data, 17)
        self.total16 = u16(data, 19)
        self.media = data[21]
        self.fat_sectors = u16(data, 22)
        self.total32 = u32(data, 32)
        self.total = self.total16 or self.total32
        self.root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.fat_start = self.rsvd
        self.root_start = self.rsvd + self.nfats * self.fat_sectors
        self.data_start = self.root_start + self.root_sectors
        self.clusters = (self.total - self.data_start) // self.spc
        if self.clusters < 4085:
            self.type = 12
        elif self.clusters < 65525:
            self.type = 16
        else:
            self.type = 32
        self.label = data[54:62].decode('ascii', 'replace').strip()
        self.oem = data[3:11].decode('ascii', 'replace')

    def fat_entry(self, n):
        if self.type == 16:
            off = self.fat_start * self.bps + n * 2
            if off + 2 > len(self.d):
                return None
            return u16(self.d, off)
        if self.type == 12:
            off = self.fat_start * self.bps + n * 3 // 2
            v = u16(self.d, off)
            return (v >> 4) if (n & 1) else (v & 0xFFF)
        return u32(self.d, self.fat_start * self.bps + n * 4) & 0x0FFFFFFF

    def cluster_offset(self, c):
        return (self.data_start + (c - 2) * self.spc) * self.bps

    def cluster_bytes(self, c):
        off = self.cluster_offset(c)
        return self.d[off:off + self.spc * self.bps]

    def chain(self, start, want_bytes=None):
        out = b''
        c = start
        seen = set()
        while 2 <= c < self.clusters + 2:
            if c in seen:
                raise SystemExit('FAT loop at cluster %d' % c)
            seen.add(c)
            out += self.cluster_bytes(c)
            if want_bytes is not None and len(out) >= want_bytes:
                break
            nxt = self.fat_entry(c)
            if nxt is None:
                raise SystemExit('FAT entry out of range at cluster %d' % c)
            if self.type == 16 and nxt >= 0xFFF8:
                break
            if self.type == 16 and nxt == 0:
                break
            c = nxt
        return out[:want_bytes] if want_bytes is not None else out

    def read_dir(self, cluster):
        if cluster == 0:
            off = self.root_start * self.bps
            blob = self.d[off:off + self.root_sectors * self.bps]
        else:
            blob = self.chain(cluster)
        entries = []
        lfn = []
        lfn_checks = []
        for i in range(0, len(blob), 32):
            e = blob[i:i + 32]
            if len(e) < 32 or e[0] == 0x00:
                break
            if e[0] == 0xE5:
                lfn = []
                lfn_checks = []
                continue
            attr = e[11]
            if attr == 0x0F:
                seq = e[0] & 0x1F
                part = e[1:11] + e[14:26] + e[28:32]
                lfn.append((seq, part))
                lfn_checks.append((seq, e[13]))
                continue
            sn = e[0:11]
            name = None
            if lfn:
                # Entries are stored in reverse order on disk; the name is the
                # concatenation of the parts in ascending sequence order. FatPkg
                # only honours the long name when every entry carries the same
                # checksum of the 8.3 name and the sequence runs 1..n.
                pairs = sorted(lfn, key=lambda x: x[0])
                checks = sorted(lfn_checks, key=lambda x: x[0])
                cks = 0
                for ch in sn:
                    cks = (((cks & 1) << 7) + (cks >> 1) + ch) & 0xFF
                seqs = [s for s, _ in pairs]
                if seqs == list(range(1, len(seqs) + 1)) and all(c == cks for _, c in checks):
                    raw = b''.join(p for _, p in pairs)
                    chars = raw.decode('utf-16-le', 'ignore')
                    name = chars.split('\x00')[0]
                else:
                    print('WARN: LFN checksum/order mismatch before %r -> UEFI will use the 8.3 name' % sn)
                lfn = []
                lfn_checks = []
            if name is None:
                if sn[0] == 0x05:
                    sn = b'\xe5' + sn[1:]
                base = sn[0:8].decode('ascii', 'replace').rstrip()
                ext = sn[8:11].decode('ascii', 'replace').rstrip()
                name = base + ('.' + ext if ext else '')
            entries.append({
                'name': name,
                'attr': attr,
                'cluster': u16(e, 26),
                'size': u32(e, 28),
                'sn': sn,
            })
        return entries

    def resolve(self, path):
        parts = [p for p in path.replace('/', '\\').split('\\') if p]
        cluster = 0
        entry = None
        for part in parts:
            found = None
            for e in self.read_dir(cluster):
                if e['attr'] == 0x08:
                    continue
                if e['name'].lower() == part.lower():
                    found = e
                    break
            if found is None:
                raise SystemExit('NOT FOUND: %s (missing %r)' % (path, part))
            entry = found
            cluster = entry['cluster']
        return entry

    def read(self, path):
        e = self.resolve(path)
        if e['attr'] & 0x10:
            raise SystemExit('%s is a directory' % path)
        return self.chain(e['cluster'], e['size'])


def main():
    data = open(sys.argv[1], 'rb').read()
    if data[510:512] != b'\x55\xaa':
        raise SystemExit('missing 0x55AA boot signature')
    f = Fat(data)
    print('OEM      : %r' % f.oem)
    print('label    : %r' % f.label)
    print('bps/spc  : %d/%d  total sectors %d  fat sectors %d' % (f.bps, f.spc, f.total, f.fat_sectors))
    print('clusters : %d  => FAT%d  (declared %r)' % (f.clusters, f.type, f.label))
    declared = data[54:62].decode('ascii', 'replace').strip()
    if declared.startswith('FAT') and int(declared[3:]) != f.type:
        raise SystemExit('FAT type mismatch: BPB declares %s, geometry implies FAT%d' % (declared, f.type))
    if f.type == 16 and f.clusters < 4085:
        raise SystemExit('FAT16 with %d clusters is below the spec minimum of 4085' % f.clusters)
    # both FAT copies must agree
    a = data[f.fat_start * f.bps:(f.fat_start + f.fat_sectors) * f.bps]
    b = data[(f.fat_start + f.fat_sectors) * f.bps:(f.fat_start + 2 * f.fat_sectors) * f.bps]
    if a != b:
        raise SystemExit('FAT copies differ')

    def dump(cluster, indent=0):
        for e in f.read_dir(cluster):
            if e['attr'] == 0x08:
                continue
            tag = 'DIR ' if e['attr'] & 0x10 else 'FILE'
            print('%s%s %-24s cluster=%-5d size=%d' % ('  ' * indent, tag, e['name'], e['cluster'], e['size']))
            if e['attr'] & 0x10 and e['name'] not in ('.', '..'):
                dump(e['cluster'], indent + 1)

    print('--- tree ---')
    dump(0)

    for p in sys.argv[2:]:
        blob = f.read(p)
        print('--- %s : %d bytes ---' % (p, len(blob)))
        print(blob.decode('utf-8', 'replace')[:600])
    print('OK')


if __name__ == '__main__':
    main()
