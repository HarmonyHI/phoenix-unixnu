# phoenix / UniXNU 项目长期笔记

## 一句话
在 Redmi K30 4G（phoenix，SM7150，Adreno 618，BL 已解锁）上，用 Mu-Silicium UEFI + UniXNU
尝试引导 XNU 内核。**只用 `fastboot boot`（RAM 启动），绝不写任何分区** —— 这是用户的硬红线。

## 当前状态（最后更新 2026-11-07 21:30，已上机验证）
- 固件层全链路已打通并真机验证：`fastboot boot` → BootShim → SEC/PEI/DXE（带 DTB）→
  `UniXNURamDiskDxe` 从固件卷解压 4 MiB FAT（`ReadSection` 4194304）→ BlockIo →
  `ConnectController` → FatPkg → SimpleFileSystem → 读 `\EFI\UniXNU\unixnu.toml` →
  UniXNU 菜单 v0.1.0 → ADTInspector 读回 `phoenix.adt`（12,368 B）→ 6 项检查全 PASS。
- 已验证镜像：`mu-silicium/Mu-phoenix-fv6.img`（5,699,584 B，11-07 21:14 构建）；
  2026-11-08 起同一份二进制已入库为 `phoenix-unixnu/prebuilt/Mu-phoenix-fv6-menu.img`
  （sha256 `191c6a57…536844`）—— clone 后 `fastboot boot` 即可复现菜单，无需工具链。
- 对外仓库：github.com/HarmonyHI/phoenix-unixnu @ `012fcc7`（源码 + 补丁 + 文档 + **一个**已验证镜像；
  `.gitignore` 全局排除 `*.img`，只有 `prebuilt/` 例外）。
- 固件树 `mu-silicium/` 有 26 个未提交改动；`Mu_Basecore` 里还留着黑屏排查的 6 处调试打印
  （`P0`–`P5`、`GSA … ra=…`）—— 有意未发布，也没从工作树清掉，下次构建仍会打这些噪音。
- ADT 里 DRAM 上界还是保守值 `0xFCC00000`，真实值待取。

## 常用命令
- 全量构建：`cd mu-silicium && ./Resources/Scripts/build-phoenix.sh Mu-phoenix-<name>.img`（约 60 秒）
- 只重打包（用现成 FD）：`./Resources/Scripts/mkinitrd.sh <out.img>`，**必须先有 DTB**（脚本已硬检查）
- 上机：`fastboot reboot-bootloader` → 等 6 秒 → `fastboot boot <img>`；连续 `Load Error` 就再
  reboot-bootloader 一次。`fastboot devices` 为空 **不等于** 固件在跑（很可能已复位进 Android）。
- 抓屏判读：异常 dump 已精简，一张照片就能看到 `PC 0x… (base+off) [i] 模块名.pdb`。

## 下一步（优先级）
1. `adb shell cat /proc/meminfo` 取真实 DRAM 上界 → `make-phoenix-adt.py --dram-size` 重新生成 ADT。
2. XNU 内核本体：本地 `unixnu/` 构建系统，参考 `UniXNU-sweet-main`（同为 SM7150）；
   XNU 10063.141.1、`ARM64_LINK_ADDR=0xfffffff007004000`、prelink 需要与本机 iOS 匹配的 kernelcache。
3. 目标口径：能打出 early boot 日志就算跨过内核入口；iOS 桌面因 Adreno 618 没有 Metal 驱动不可达。

## 文件地图
- `phoenix-unixnu/`：对外仓库（docs/ port/ overlay/ patches/ dts/ dtb/ scripts/apply-to-mu-silicium.sh）
- `phoenix-unixnu/dtb/phoenix-initrd.dtb`：**唯一不可重建的输入**（398,781 B，来自原厂 boot image，
  `dts/` 编不出来），2026-11-08 起随仓库提供，apply 脚本会放到固件根目录
- `mu-silicium/UniXNU/UniXNUPkg/Driver/UniXNURamDiskDxe/`：本阶段核心驱动
- `mu-silicium/Resources/Scripts/{mkfat.py,checkfat.py,build-phoenix.sh,mkinitrd.sh}`
- `PORTING-PHOENIX.md`：总路线图与阶段计划（第 7 节是下一步）
- `mu-silicium/Platforms/Xiaomi/phoenixPkg/`：dsc / fdf / MemoryMapLib / APRIORI / DXE

## 三个最容易再踩的坑
1. boot image 必须在 gzip 内核后追加 `phoenix-initrd.dtb`，否则 SEC 死在 MMU 初始化（症状：孤零零一行
   `splitting block entry with MMU disabled` 然后复位）。
2. `Fv2->GetNextFile` 的 `FileType` 是**输入过滤**（须置 0），`Key` 必须是 **`UINTN`**（8 字节）。
3. 早期诊断绝不能读 HYP `0x85700000-0x86000000` 与 QTEE/PIL `0x86200000-0x98500000` —— 读即静默冻屏。
