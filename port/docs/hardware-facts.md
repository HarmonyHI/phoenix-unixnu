# phoenix hardware facts

Everything here was read out of the device tree that the phone's own ABL hands
over, or out of firmware code that has actually run on the device. Values are
recorded with the evidence they came from, so the ADT generator stays
reviewable instead of magical.

Source blob: `mu-silicium/phoenix-trimmed.dtb` (397,929 bytes, FDT v17,
`model = "Qualcomm Technologies, Inc. SDMMAGPIE SoC"`,
`compatible = "qcom,sdmmagpie"`).

## Interrupt controller

```
/soc/interrupt-controller@17a00000
  compatible = "arm,gic-v3"
  reg = <0x17a00000 0x10000>, <0x17a60000 0x100000>
```

The ADT describes this inside `/arm-io`, whose `ranges` maps CPU address 0 to
`ARMIO_BASE = 0x01000000`, so the encoded offsets are `0x16a00000` and
`0x16a60000`. `check-phoenix-adt.py` verifies that adding `ARMIO_BASE` back
yields `0x17A00000`.

`redistributor-stride = 0x20000` and `virtual-timer-intid = 19` follow the
generic GICv3 layout for this SoC family and match the sweet port.

## CPUs

```
/cpus  #address-cells = <2>; #size-cells = <0>
/cpus/cpu@0 .. cpu@700
  device_type = "cpu"
  compatible = "arm,armv8"
  reg = <0x0 0x0> .. <0x0 0x700>
  enable-method = "psci"
/psci
  compatible = "arm,psci-1.0"
  method = "smc"
```

Eight cores with affinities `0, 0x100, ... 0x700`; the DT does not split
little/big, so the ADT uses the SM7150 part's 6x Cortex-A55 + 2x Cortex-A76
split with the two big cores last (`cpu6`, `cpu7`), which this SoC places at
affinity `0x600`/`0x700`.

## Memory

The `/memory` node's `reg` is zeroed in the trimmed blob, so the DRAM extent
is **not** taken from it. Reserved-memory children still carry real
addresses and bound the low bank:

| region | start | size |
| --- | --- | --- |
| hyp_region | 0x085700000 | 6 MiB |
| xbl_aop_mem | 0x085e00000 | 2 MiB |
| smem | 0x086000000 | 2 MiB |
| removed_region | 0x086200000 | 45 MiB |
| camera_region | 0x08ab00000 | 5 MiB |
| modem_region | 0x08b000000 | 132 MiB |
| pil_video_region | 0x093400000 | 5 MiB |
| cdsp_regions | 0x093900000 | 30 MiB |
| pil_adsp_region | 0x095700000 | 38 MiB |
| wlan_msa_region | 0x097d00000 | 1.5 MiB |
| npu_region | 0x097e80000 | 0.5 MiB |
| ips_fw_region | 0x097f00000 | 64 KiB |
| ipa_gsi_region | 0x097f10000 | 20 KiB |
| gpu_region | 0x097f15000 | 8 KiB |
| cont_splash_region | 0x09c000000 | 23 MiB |
| disp_rdump_region | 0x09c000000 | 24 MiB |
| dfps_data_region | 0x09d700000 | 1 MiB |
| qseecom_region | 0x09e400000 | 20 MiB |
| cdsp_sec_regions | 0x09f800000 | 30 MiB |

Highest low-bank reservation ends at `0x0A1600000`, so the low bank extends at
least that far. The upper bank starts at the SM7150-standard `0x100000000`.

`dram-size` therefore stays an explicit argument (`0xfcc00000` default, the
same extent the sweet port records for a 6 GiB SM7150). Under-reporting only
costs usable memory; over-reporting would let XNU touch unmapped addresses.
Override with `--dram-size` once the phone's live bank end is measured
(`adb shell cat /proc/meminfo` or a firmware memory-map dump).

## Payload handoff observations (from real boots)

* `/chosen` in the ABL-supplied blob contains only `bootargs`, **no**
  `linux,initrd-start` / `linux,initrd-end`. The initrd path through the DT
  is therefore dead on this device and the payload cannot rely on it.
* Mu-Silicium's `_ModuleEntryPoint` clobbers `x0` immediately, so the ABL FDT
  pointer is lost before any C code runs. Attempts to stash it in SEC caused
  a black screen with no output at all, so that change was reverted and lives
  only in this note.
* The first FAT hit during a DRAM scan was `0x85FFF800`, inside the
  `sec_apps_region` gap - not the payload. Scanning must skip unmapped holes
  (`0x84920000-0x85700000`, `0x98500000-0x9C000000`) or it takes a data abort.
* Therefore the payload volume is embedded in the firmware FV and published
  from there, with a DRAM signature scan only as a fallback.

## Touch / display (not yet used by the port)

```
/soc/qcom,mdss_mdp@ae00000   -> base 0xae00000
/soc/qcom,kgsl-3d0@5000000   -> base 0x5000000  (Adreno 618, A618)
/soc/ufshc@1d84000           -> base 0x1d84000  size 0x3000, second region 0x1d90000
```

The `reg` properties decode as normal 2-cell address / 2-cell size pairs; read
them as four `u32` values, not as two `u64`s.
