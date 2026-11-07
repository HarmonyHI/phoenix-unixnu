#!/usr/bin/env python3
"""Host-side regression tests for the phoenix ADT generator and validator.

Runs the generator, validates the result, then corrupts specific fields and
asserts that the validator refuses them. This proves the validator is not a
rubber stamp.
"""
import subprocess
import sys
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / 'scripts' / 'make-phoenix-adt.py'
CHECK = ROOT / 'scripts' / 'check-phoenix-adt.py'
TMP = Path('/tmp/phoenix-adt-tests')


def run(*args, expect=0):
    result = subprocess.run([sys.executable, *map(str, args)],
                            capture_output=True, text=True)
    if result.returncode != expect:
        print(result.stdout)
        print(result.stderr)
        raise SystemExit('FAIL: %s exited %d, expected %d' %
                         (' '.join(map(str, args)), result.returncode, expect))
    return result


def find_prop_value(blob, key):
    """Return the offset of a property's payload given its name."""
    padded = key.encode() + b'\0' * (32 - len(key))
    start = 0
    hits = []
    while True:
        at = blob.find(padded, start)
        if at < 0:
            break
        length = struct.unpack_from('<I', blob, at + 32)[0]
        hits.append((at + 36, length))
        start = at + 1
    if not hits:
        raise SystemExit('FAIL: property %r not found' % key)
    return hits


def main():
    TMP.mkdir(parents=True, exist_ok=True)
    good = TMP / 'good.adt'

    print('[1] generate + validate')
    run(GEN, '--output', good)
    out = run(CHECK, good)
    assert 'OK:' in out.stdout, out.stdout

    blob = bytearray(good.read_bytes())

    print('[2] corrupt dram-base -> validator must fail')
    offsets = find_prop_value(blob, 'dram-base')
    at, length = offsets[0]
    assert length == 8, length
    broken = bytearray(blob)
    struct.pack_into('<Q', broken, at, 0x200000000)
    path = TMP / 'bad-dram-base.adt'
    path.write_bytes(broken)
    out = run(CHECK, path, expect=1)
    assert 'FAIL' in out.stdout and 'dram-base' in out.stdout, out.stdout

    print('[3] corrupt cpu0 affinity -> validator must fail')
    offsets = find_prop_value(blob, 'reg')
    at, length = offsets[0]
    broken = bytearray(blob)
    struct.pack_into('<I', broken, at, 0x900)
    path = TMP / 'bad-cpu-reg.adt'
    path.write_bytes(broken)
    out = run(CHECK, path, expect=1)
    assert 'FAIL' in out.stdout and 'affinity' in out.stdout, out.stdout

    print('[4] truncate blob -> parser must reject')
    path = TMP / 'truncated.adt'
    path.write_bytes(bytes(blob[:len(blob) // 2]))
    run(CHECK, path, expect=1)

    print('[5] generator refuses non-monotonic affinities')
    run(GEN, '--output', TMP / 'bad.adt', '--cpu-regs', '0,0x200', expect=2)

    print('[6] generator refuses owned memory beyond dram')
    run(GEN, '--output', TMP / 'bad2.adt', '--dram-size', '0x10000000',
        '--owned-memory-size', '0x20000000', expect=2)

    print('\nall ADT tests passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
