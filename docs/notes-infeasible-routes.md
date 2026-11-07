# phoenix port handoff notes

## Current milestone

Platform-stage port. The firmware side works far enough that the UniXNU menu
appears on the phone, but the payload volume has never been read back from
inside the OS yet. The immediate goal is to close that loop with a payload that
reports what it found, not to boot XNU.

## What is actually verified, and how

* **Firmware boots on hardware.** `fastboot boot` of the Mu-Silicium phoenix
  build reaches the UniXNU menu with an Apple logo and `v0.1.0`. Verified by
  photographing the screen.
* **FAT payload images are spec-correct.** `fsck_msdos` reports clean, and
  `Resources/Scripts/checkfat.py` walks the image the way FatPkg does
  (geometry, FAT type, duplicate-FAT agreement, LFN checksum and ordering,
  cluster chains, case-insensitive path resolution).
* **The payload is embedded in the firmware FV.** FFS FREEFORM file with a
  Tiano compressed section, uncompressed length `0x400004`, compression type 1.
  Verified by parsing `FVMAIN.Fv` directly. This removes the dependency on the
  bootloader placing the ramdisk at a known address.
* **ADT generation and validation.** `tests/test-adt.py` covers six cases, three
  of which deliberately corrupt the blob or the arguments and assert refusal.
* Nothing else. In particular no kernel has run, and no driver outside the
  firmware has touched hardware.

## Dead ends that should not be retried

1. **`linux,initrd-start` in `/chosen`.** The ABL-supplied blob contains only
   `bootargs`. There is no initrd property to read, so the
   "ramdisk via device tree" path is dead on this device.
2. **Stashing `x0` in SEC.** `_ModuleEntryPoint` clobbers it before any C code,
   so a hook was added that saved it from the assembly entry. It produced a
   completely black screen with no firmware output at all, twice, with
   relocation verified correct in the final FD. Reverted. The UniXNU side now
   scans for the `UNIXNU00` signature and falls back to the embedded FV copy.
3. **Full-DRAM signature scans.** The first hit was `0x85FFF800`, inside a
   reserved gap, not the payload. Scanning unmapped holes
   (`0x84920000-0x85700000`, `0x98500000-0x9C000000`) takes a data abort and
   the phone resets. Any scan must be limited to mapped ranges.
4. **`UFSDxe` and `PILDxe`.** Both are removed from the phoenix APRIORI/DXE
   lists. `UFSDxe` hangs in init and trips the watchdog; `PILDxe` jumps to a
   null address when `uefipil.cfg` cannot be read. Without storage there is no
   reason to have either.
5. **UEFI Shell as the payload.** It asserts in `ShellLibConstructor` before
   any protocol is installed, and even after patching that, takes a data abort
   in the minimal environment. Not worth the effort; a purpose-built
   application is smaller and predictable.

## Next concrete steps

1. Flash `Mu-phoenix-adt.img` and photograph the `UNIXNU-ADT:` block. That
   closes the "payload volume readable" question either way.
2. If the volume reads: measure the real upper DRAM bank and regenerate the ADT
   with the correct `--dram-size`.
3. If it does not read: the fallback chain in `UniXNURamDiskDxe`
   (FDT → DRAM scan → embedded FV section) needs per-stage logging compared
   against the three sources, and the FV `ReadSection` path is the one to make
   authoritative.
4. Only then consider a kernel. That needs Xcode 26.2, an Apple source drop and
   a matching kernelcache for prelink, none of which are present here.

## Environment notes

* `hdiutil` is blocked by host policy, hence the hand-written FAT builder.
* Post-build PDB flattening unlinks files and is aborted by the host's delete
  guard after a successful compile. `build-phoenix.sh` sets
  `EDK2_SKIP_FLATTEN_PDBS=1`; the plugin honours it.
* The build must run with `cd` inside the firmware tree in the same command;
  the shell resets its working directory between tool calls.
