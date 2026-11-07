#!/usr/bin/env python3
"""Parse and validate a UniXNU ADT blob produced by make-phoenix-adt.py.

Apple's device tree format is not FDT: each node is

    u32 property_count, u32 child_count,
    property_count x { char name[32], u32 length, data padded to 4 },
    then child_count child nodes

This tool re-walks the blob, prints a summary and asserts the invariants the
kernel loader depends on, so a bad generator change fails loudly on the host
instead of on the phone.
"""
import struct
import sys


def align4(value):
    return (value + 3) & ~3


def parse_node(blob, offset=0):
    if offset + 8 > len(blob):
        raise ValueError('truncated node header at %#x' % offset)
    prop_count, child_count = struct.unpack_from('<II', blob, offset)
    offset += 8
    if prop_count > 256 or child_count > 256:
        raise ValueError('implausible node counts %d/%d at %#x' %
                         (prop_count, child_count, offset - 8))
    props = {}
    order = []
    for _ in range(prop_count):
        if offset + 36 > len(blob):
            raise ValueError('truncated property at %#x' % offset)
        raw_name = blob[offset:offset + 32]
        name = raw_name.split(b'\0')[0].decode('utf-8', 'replace')
        length = struct.unpack_from('<I', blob, offset + 32)[0]
        offset += 36
        if length > len(blob):
            raise ValueError('property %r length %d exceeds blob' % (name, length))
        value = blob[offset:offset + length]
        if len(value) != length:
            raise ValueError('property %r truncated' % name)
        offset += align4(length)
        props[name] = value
        order.append(name)
    children = {}
    for _ in range(child_count):
        child, offset = parse_node(blob, offset)
        name = child['props'].get('name', b'').split(b'\0')[0].decode('utf-8', 'replace')
        children[name] = child
    return {'props': props, 'order': order, 'children': children}, offset


def u32(blob):
    return struct.unpack('<I', blob)[0]


def u64(blob):
    return struct.unpack('<Q', blob)[0]


def main(path):
    blob = open(path, 'rb').read()
    root, consumed = parse_node(blob)
    if consumed != len(blob):
        raise SystemExit('FAIL: parsed %d of %d bytes, trailing data' % (consumed, len(blob)))

    chosen = root['children']['chosen']['props']
    cpus = root['children']['cpus']
    arm_io = root['children']['arm-io']
    intc = arm_io['children']['intc']['props']
    product = root['children']['product']['props']

    print('file            : %s (%d bytes)' % (path, len(blob)))
    print('model           : %s' % root['props']['model'].split(b'\0')[0].decode())
    print('compatible      : %s' % root['props']['compatible'].replace(b'\0', b' | ').decode())
    print('product         : %s / %s' % (
        product['product-name'].split(b'\0')[0].decode(),
        product['model'].split(b'\0')[0].decode()))
    print('dram base/size  : %#x / %#x' % (u64(chosen['dram-base']), u64(chosen['dram-size'])))
    print('darwinboot mem  : %#x, range %#x+%#x' % (
        u64(chosen['darwinboot-memory-size']),
        u64(chosen['darwinboot-memory-range'][:8]),
        u64(chosen['darwinboot-memory-range'][8:])))
    print('nvram bank      : %d byte(s) x%d' % (
        u32(chosen['nvram-bank-size']), u32(chosen['nvram-bank-count'])))

    cpu_names = [n for n in cpus['children'] if n.startswith('cpu')]
    print('cpus            : %d (%s)' % (len(cpu_names), ', '.join(cpu_names)))
    for name in cpu_names:
        props = cpus['children'][name]['props']
        print('  %-6s %-18s reg=%#x state=%s tb=%d' % (
            name,
            props['compatible'].split(b'\0')[0].decode(),
            u32(props['reg']),
            props['state'].split(b'\0')[0].decode(),
            u32(props['timebase-frequency'])))

    reg = struct.unpack('<4Q', intc['reg'])
    print('gic             : dist %#x/%#x, redist %#x/%#x, stride %#x, vtimer intid %d' % (
        reg[0], reg[1], reg[2], reg[3],
        u64(intc['redistributor-stride']), u32(intc['virtual-timer-intid'])))

    # Invariants the loader relies on.
    failures = []
    if u32(chosen['nvram-bank-size']) != len(chosen['nvram-proxy-data']):
        failures.append('nvram-bank-size does not match nvram-proxy-data length')
    if u64(chosen['darwinboot-memory-size']) > u64(chosen['dram-size']):
        failures.append('darwinboot memory larger than dram-size')
    if u64(chosen['dram-base']) != 0x100000000:
        failures.append('dram-base is not the SM7150 upper bank')
    if len(cpu_names) < 1:
        failures.append('no cpus described')
    if not cpu_names or u32(cpus['children'][cpu_names[0]]['props']['reg']) != 0:
        failures.append('first cpu affinity is not 0')
    if intc['compatible'].split(b'\0')[0] != b'arm,gic-v3':
        failures.append('intc is not arm,gic-v3')
    if struct.unpack('<Q', intc['reg'][:8])[0] + 0x01000000 != 0x17A00000:
        failures.append('gic distributor offset does not resolve to 0x17A00000')
    if len(chosen['random-seed']) != 64:
        failures.append('random-seed is not 64 bytes')

    if failures:
        print('\nFAIL')
        for item in failures:
            print('  - ' + item)
        return 1
    print('\nOK: %d nodes walked, all invariants hold' % (len(root['children']) + 1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
