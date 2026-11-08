# 已验证镜像（唯一入库的二进制）

这个目录里放的是**在真机上跑通过的那个 `boot.img`**，不是构建产物快照。
目的是让任何人 clone 下来就能直接看到 UniXNU 菜单，不必先准备工具链、拉 5 GiB 上游树。

```
prebuilt/Mu-phoenix-fv6-menu.img
  size    5,699,584 bytes (5.44 MiB)
  sha256  191c6a579f139affd2e10bf65d297a0d9ffd8f5c991c310803e87815cb536844
  built   2026-11-07
  tested  2026-11-07，Redmi K30 4G (phoenix)，实机
```

## 怎么用

```sh
fastboot devices                     # 确认设备在 bootloader
fastboot reboot-bootloader && sleep 6
fastboot boot prebuilt/Mu-phoenix-fv6-menu.img
```

约 10 秒后屏幕出现 UniXNU 菜单：Apple logo、`version 0.1.0`、
`config: \EFI\UniXNU\unixnu.toml`，下列四个条目：

```
  phoenix ADT inspector
  Hello payload
  XNU kernel
  Return to firmware
```

选第一项会打印 8 行 `UNIXNU-ADT:`，最后一行是：

```
UNIXNU-ADT: RESULT PASS 6 passed 0 failed
```

这一屏同时验证了整条链路：FV 里的内嵌 FAT 载荷卷被解出、FAT 驱动挂上
SimpleFileSystem、长文件名可解析、`\EFI\UniXNU\phoenix.adt` 被完整读出且结构自洽。

**只 `fastboot boot`，不写任何分区。** 断电或 `fastboot reboot` 就回到 Android，
设备状态不变。刷机模式下 `fastboot flash` 用这个文件是**错误用法**，别这么做。

## 里面装了什么

```
boot header v1 (page 2048, kernel_offset 0x8000, ramdisk_offset 0x0AB00000)
├─ gzip kernel = Mu-Silicium UEFI 固件 FV（BootShim 从 0x9FC00000 接管）
│    └─ 内层 FvMain（LZMA）
│         └─ FFS FREEFORM 载荷段 = 4 MiB FAT 镜像
│              └─ \EFI\UniXNU\{unixnu.toml, phoenix.adt, Payloads\*.efi}
└─ 尾部附加 phoenix-initrd.dtb (398,781 B)
```

尾部那 398,781 字节的 DTB 是硬性要求：SEC 阶段要拿它建 MMU，缺了整个镜像只打一行
`splitting block entry with MMU disabled` 就复位，看起来像驱动 bug，其实只是缺 DTB。

## 它对应哪份源码

由本仓库 overlay + `patches/0001` 落到 Mu-Silicium `8471e47f` 后，跑
`./Resources/Scripts/build-phoenix.sh` 生成。重建命令见主 README「从克隆到菜单」。

一致性做过两种校验：

- overlay 的 64 个文件与出镜像时的工作树**逐字节相同**（`cmp` 全过）；
- `patches/0001` 应用后的 12 个关键文件（dsc / fdf / APRIORI / DXE.inc / MemoryMapLib /
  BootManager / BootManagerLib.inf / UniXNURamDiskDxe / mkinitrd.sh / mkfat.py /
  build-phoenix.sh / DeviceBuild.py）与工作树逐字节相同。

也就是说：这份二进制和仓库里的源码是同一状态，不是某个被改过的本地版本。
重建出来的镜像不会与这份**逐字节**相同（FV 里有构建时间戳与随机 FV 头字段），
但屏幕行为应当一致。

## 已知限制

- 里面**没有 iOS 内核**。菜单第四项 `XNU kernel` 是空位——XNU 还没构建出来，
  见 `docs/PORTING-PHOENIX.md`。这个镜像证明的是"UEFI → 载荷卷 → 用户态程序"这段
  通了，不是"iOS 能开机"。
- ADT 里的 DRAM 上界用的是保守值 `0xFCC00000`，不是实测值。取值影响不大（只用于
  ADT 自洽性检查），拿到 `/proc/meminfo` 后重新生成即可。
- 只在 `phoenix`（Redmi K30 4G / SM7150）上验证过。其他 SM7150 机型没有测试，
  硬件映射不同，别直接套用。
