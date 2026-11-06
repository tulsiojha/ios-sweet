#!/usr/bin/env python3
"""Build the minimal Apple Device Tree for the vivo Y21L (msm8916) xnu boot.

Mirrors tools/mkadt.py (sweet) with msm8916 facts from the live QC DT
(vivo.dtb) and this payload's hardware findings. Provisional where the
kernel baseline is still undecided (GICv2 node shape, root identity).
Usage:  mkadt.py OUT.adt [--dram-base HEX] [--dram-size HEX] [--dump]
"""
import argparse, hashlib, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import adt  # noqa: E402

# Kernel-managed RAM for now: the upper 896 MB of the single 1 GB bank.
# The low 128 MB keeps stage0 (0x8000a000+), LK's DT (0x81e00000) and the
# SWEETXNU payload (0x82000000). Provisional until carve-outs are mapped.
PHYS_BASE = 0x88000000
MEM_SIZE = 0x38000000
PANIC_LOG_SIZE = 0x100000

# QGIC (GICv2): our payload proved NS physical timer = PPI 19 (msm8916 remap).
GICD, GICC = 0x0b000000, 0x0b002000
GICD_SIZE, GICC_SIZE = 0x1000, 0x2000
VTIMER_INTID = 27	# standard GICv2 virtual timer PPI on this SoC is 20; 27 is the Arm default - settle with the kernel
TIMEBASE = 19200000
SOC_BASE = 0x01000000	# /arm-io children are relative to this (like sweet)

CPU_CLOCK = 1209600000	# 4x Cortex-A53 @ 1.2 GHz (msm8916)
VIVO_ECID = 0x5649564F00000001  # 'VIVO' + 1, synthetic
CHIP_ID = 0x8010	# iPhone 6 class SoC id; provisional, follows the userland target
BOOT_MANIFEST_HASH = hashlib.sha384(b'vivo boot manifest').digest()

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(dram_base=PHYS_BASE, dram_size=MEM_SIZE):
    panic_base = dram_base + dram_size - PANIC_LOG_SIZE
    mem_size = dram_size - PANIC_LOG_SIZE
    seed = os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(dram_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('vivo-stage0')),
        ('system-firmware-version', cstr('vivo-stage0')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(2)),
        ('unique-chip-id', struct.pack('<Q', VIVO_ECID)),
        ('chip-id', u32(CHIP_ID)),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # 4x Cortex-A53, one cluster, shared L2: MPIDR Aff0 = core, Aff1 = 0.
    cpu_nodes = []
    for core in range(4):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,msm8916') + cstr('ARM,v8')),
            ('reg', u32(core)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(0)),
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(CPU_CLOCK)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(0)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    # GICv2: distributor + CPU interface. The exact property set xnu's
    # AppleARM GIC code wants for a v2 part gets settled with the kernel
    # baseline decision (iOS 12 xnu-4903 still carries the A8 driver).
    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic')),
        ('interrupt-controller', b''),
        ('#interrupt-cells', u32(3)),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICC - SOC_BASE, GICC_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('vivo-io')),
        ('compatible', cstr('vivo,msm8916') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic, node('wdt', [ph(), ('no-pmu', u32(1))])])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    return node('device-tree', [
        ph(),
        ('compatible', cstr('iPhone7,2') + cstr('vivo,msm8916') + cstr('AppleARM')),
        ('model', cstr('iPhone7,2')),
        ('target-type', cstr('n61')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, pram])


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--dram-base', default=hex(PHYS_BASE))
    ap.add_argument('--dram-size', default=hex(MEM_SIZE))
    ap.add_argument('--dump', action='store_true')
    a = ap.parse_args()
    root = build(int(a.dram_base, 0), int(a.dram_size, 0))
    blob = adt.serialize(root)
    open(a.out, 'wb').write(blob)
    if a.dump:
        adt.dump(adt.parse(blob)[0])
    print(f'{a.out}: {len(blob)} bytes', file=sys.stderr)