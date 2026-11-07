# UniXNU-phoenix

Platform-stage UniXNU port for the **Xiaomi Redmi K30 4G** (`phoenix`,
Qualcomm SM7150 / Snapdragon 730G, Adreno 618).

This tree holds the board description, the ADT generator and the host-side
tests. It does **not** contain the XNU sources or Apple components; those come
from the upstream UniXNU tree, and none of them is prelinked here yet.

The firmware this port targets is the project's own Mu-Silicium phoenix build
(`mu-silicium/Platforms/Xiaomi/phoenixPkg`), booted with `fastboot boot` only.
Nothing is written to the device and no Android partition is touched.

## Status

| item | state |
| --- | --- |
| ADT generator (`scripts/make-phoenix-adt.py`) | working, generates a 12 KiB blob |
| ADT validator (`scripts/check-phoenix-adt.py`) | working, enforces 8 invariants |
| Host regression tests (`tests/test-adt.py`) | 6/6 passing, 3 of them negative |
| Board description (`board.toml`) | written, `stage = "platform"` |
| Kernel defconfig (`configs/phoenix_defconfig`) | written, never built |
| UEFI payload hand-off (FAT volume → menu → EFI payload) | implemented, needs one device run |
| Real XNU kernel | not built; needs Xcode 26.2 and an Apple source drop |
| Display, touch, UFS, GPU | not started |

Nothing in this port has run on hardware yet except the firmware side that the
parent project already validated.

## Layout

```
board.toml                    board / payload description consumed by the port tooling
configs/phoenix_defconfig     source-kernel platform bring-up config
scripts/make-phoenix-adt.py   generates the Apple device tree for phoenix
scripts/check-phoenix-adt.py  parses the blob back and checks invariants
tests/test-adt.py             host regression suite (positive + negative)
docs/hardware-facts.md        every hardware constant with the evidence behind it
```

## Using it

Generate and validate an ADT:

```sh
python3 scripts/make-phoenix-adt.py --output out/phoenix.adt
python3 scripts/check-phoenix-adt.py out/phoenix.adt
python3 tests/test-adt.py
```

Override the DRAM extent once the phone's real upper bank is known:

```sh
python3 scripts/make-phoenix-adt.py --output out/phoenix.adt --dram-size 0x1fcc00000
```

Build the firmware image that carries the ADT (from the firmware tree):

```sh
cd ../mu-silicium
./Resources/Scripts/build-phoenix.sh Mu-phoenix-adt.img
fastboot boot Mu-phoenix-adt.img
```

`build-phoenix.sh` calls this tree's generator automatically when the port tree
sits next to the firmware tree, or wherever `PHOENIX_PORT_DIR` points.

## What the phone should show

The payload volume is embedded in the firmware, so the boot does not depend on
the bootloader placing the ramdisk correctly. After the UniXNU menu the default
entry runs `Payloads/ADTInspector.efi`, which prints:

```
UNIXNU-ADT: read NNNN bytes from \EFI\UniXNU\phoenix.adt
UNIXNU-ADT: check file PASS (...)
UNIXNU-ADT: check parse PASS (...)
UNIXNU-ADT: check model PASS (model 'phoenix-platform-bringup')
UNIXNU-ADT: check dram PASS (dram 100000000 + fcc00000 (owned 40000000))
UNIXNU-ADT: check product PASS (product 'Redmi K30 4G')
UNIXNU-ADT: check cpus PASS (8 cpu nodes)
UNIXNU-ADT: RESULT PASS 6 passed 0 failed
```

That single screen proves the payload volume mounted, the FAT driver resolved a
long file name, the file was read whole, and the ADT the port generates is
structurally sound. It is the last checkpoint before a real kernel is needed.

## Deliberate non-goals for now

* No Apple identity, no prelink, no userdata layout. Those need an original
  kernelcache matching a specific iOS build, which this port has not qualified.
* No `NativeUfs`. Upstream's implementation targets SM8550; phoenix storage is
  reachable but unused at this stage, and the firmware deliberately drops
  `UFSDxe` because it hangs on this device without a storage stack.
* No GPU bring-up. The Adreno 618 needs its own KMD and firmware; nothing in
  the current stage touches `kgsl`.
