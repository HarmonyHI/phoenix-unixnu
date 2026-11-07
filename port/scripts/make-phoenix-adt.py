#!/usr/bin/env python3
"""Generate the UniXNU platform-stage ADT for phoenix (Redmi K30 4G, SM7150).

This is derived from the sweet port's generator. Every constant below was
cross-checked against the phoenix device tree that the phone's own ABL hands
to the kernel (see docs/hardware-facts.md), not copied from sweet:

  * GIC distributor  0x17A00000 / 0x10000   (dt: interrupt-controller@17a00000)
  * GIC redistributor 0x17A60000 / 0x100000 (dt: same node, second region)
  * CPUs are described as arm,armv8 with enable-method "psci" in the DT, and
    /psci says arm,psci-1.0 with method "smc".
  * The DT's /memory reg is zeroed in the trimmed blob we have, so dram-size
    stays an explicit, reviewable argument instead of a guess.

No device access and no Apple input is needed. The output describes real
phoenix hardware without claiming an Apple product identity.
"""
import argparse
from pathlib import Path
import os
import struct
import zlib

# Upper DRAM bank. Qualcomm SM7150 maps the bulk of RAM above 4 GiB.
DRAM_BASE = 0x100000000
# Conservative default: the same upper-bank extent the sweet port records for
# a 6 GiB SM7150. Under-reporting only costs usable memory; over-reporting
# would let XNU touch unmapped addresses. Override with --dram-size once the
# phone's real bank end has been measured.
DRAM_SIZE = 0xFCC00000
ARMIO_BASE = 0x01000000
GICD = 0x17A00000
GICR = 0x17A60000
TIMEBASE = 19200000
TIMER_INTID = 19

# phoenix is a 2x Cortex-A76 + 6x Cortex-A55 SM7150 part.
BIG_CORES = 2


def u32(value):
    return struct.pack('<I', value)


def u64(*values):
    return struct.pack('<' + 'Q' * len(values), *values)


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = bytearray(struct.pack('<II', len(properties), len(children)))
    for key, value in properties:
        encoded = key.encode()
        if len(encoded) > 31:
            raise ValueError('ADT property name exceeds 31 bytes: %r' % key)
        result += struct.pack('<32sI', encoded, len(value))
        result += value + bytes(-len(value) % 4)
    return bytes(result) + b''.join(children)


def empty_nvram_proxy():
    # Volatile CHRP-format bank, identical in shape to UniXNU's QEMU fixture.
    # Nothing is written to the phone's NVRAM or to UFS; IODTNVRAM validates it.
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', u32(len(image))),
            ('nvram-bank-count', u32(1)), ('nvram-current-bank', u32(0)),
            ('nvram-proxy-data', bytes(image))]


def build(*, cpu_count=1, owned_memory_size=0x40000000, dram_size=DRAM_SIZE,
          seed=None):
    if cpu_count not in range(1, 9):
        raise ValueError('cpu count must be 1..8')
    if not 0x10000000 <= owned_memory_size <= dram_size or owned_memory_size % 0x200000:
        raise ValueError('owned memory must fit the upper bank, in 2 MiB steps, at least 256 MiB')
    seed = os.urandom(64) if seed is None else seed
    if len(seed) != 64:
        raise ValueError('random seed must contain 64 bytes')

    little = cpu_count - BIG_CORES
    cpus = []
    for core in range(cpu_count):
        is_big = core >= little
        cpus.append(node(f'cpu{core}', [
            ('device_type', b'cpu\0'),
            ('compatible', b'arm,cortex-a76\0' if is_big else b'arm,cortex-a55\0'),
            # MPIDR affinity, matching the DT's cpu@0..cpu@700 naming.
            ('reg', u32(core << 8)),
            ('state', b'running\0' if core == 0 else b'waiting\0'),
            ('timebase-frequency', u32(TIMEBASE)),
        ]))

    return node('device-tree', [
        ('model', b'phoenix-platform-bringup\0'),
        ('compatible', b'xiaomi,phoenix\0qcom,sm7150\0'),
        ('#address-cells', u32(2)), ('#size-cells', u32(2)),
    ], [
        node('chosen', [
            ('random-seed', seed),
            ('dram-base', u64(DRAM_BASE)), ('dram-size', u64(dram_size)),
            ('darwinboot-memory-size', u64(owned_memory_size)),
            ('darwinboot-memory-range', u64(DRAM_BASE, dram_size)),
            ('crypto-hash-method', b'sha2-384\0'),
            *empty_nvram_proxy(),
        ], [node('memory-map'), node('manifest-properties')]),
        node('cpus', [('#address-cells', u32(1)), ('#size-cells', u32(0))], cpus),
        node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', b'smc\0')]),
        node('defaults', [('sleep-disabled', u32(1))]), node('options'),
        node('product', [('product-name', b'Redmi K30 4G\0'),
                         ('model', b'xiaomi,phoenix\0')]),
        node('arm-io', [
            ('device_type', b'phoenix-io\0'),
            ('#address-cells', u32(2)), ('#size-cells', u32(2)),
            ('ranges', u64(0, ARMIO_BASE, 0x100000000 - ARMIO_BASE)),
        ], [node('intc', [
            ('compatible', b'arm,gic-v3\0'), ('interrupt-controller', b''),
            ('reg', u64(GICD - ARMIO_BASE, 0x10000, GICR - ARMIO_BASE, 0x100000)),
            ('redistributor-stride', u64(0x20000)),
            ('virtual-timer-intid', u32(TIMER_INTID)),
        ])]),
    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dram-size', type=lambda v: int(v, 0), default=DRAM_SIZE,
                        help='upper-bank size, default %#x (6 GiB-class SM7150)' % DRAM_SIZE)
    parser.add_argument('--owned-memory-size', type=lambda v: int(v, 0),
                        default=0x40000000,
                        help='memory handed to Darwin, default 1 GiB')
    parser.add_argument('--cpu-regs', default='0,0x100,0x200,0x300,0x400,0x500,0x600,0x700',
                        help='affinity prefix; must be 0,0x100,0x200,...')
    args = parser.parse_args()

    try:
        regs = [int(v, 0) for v in args.cpu_regs.split(',')]
        if not regs or regs != [core << 8 for core in range(len(regs))]:
            raise ValueError('--cpu-regs must be the affinity prefix 0,0x100,...')
        blob = build(cpu_count=len(regs),
                     owned_memory_size=args.owned_memory_size,
                     dram_size=args.dram_size)
    except ValueError as error:
        parser.error(str(error))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print('%s: %d bytes, %d CPUs, dram %#x, owned %#x' %
          (args.output, len(blob), len(regs), args.dram_size, args.owned_memory_size))
    print('platform-stage ADT; not hardware validated')


if __name__ == '__main__':
    main()
