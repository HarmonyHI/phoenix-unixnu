# Redmi K30 4G (phoenix / SM7150) → UniXNU 移植路线图

> 状态：立项调研完成（2026-11-06）。目标：在 phoenix 上通过 UEFI + XNU 引导 iOS 用户态。

## 0. 设备实测数据（fastboot 采集，2026-11-06）

| 项目 | 值 |
| --- | --- |
| 代号 | `phoenix`（Redmi K30 4G，Poco X2） |
| SoC | SM7150（骁龙 730G），Kryo 470 = 2×A76@2.2G + 6×A55@1.8G |
| GPU | **Adreno 618**（与 sweet/Redmi Note 10 Pro 同款） |
| 存储 | UFS（`variant: SM7 UFS`），4 KiB 逻辑块 |
| Bootloader | 已解锁（`unlocked: yes`），anti-rollback = 2 |
| 分区方案 | 非 A/B（`getvar current-slot` 不支持），有 `super`（动态分区），无 `vendor_boot` → boot header v2 |
| boot | 0x8000000（128 MiB） |
| dtbo | 0x2000000（32 MiB） |
| **userdata** | **0x1AC55FB000 = 114,715,564,032 B ≈ 106.86 GiB（128G 版实测）** |
| super | 0x220000000（8.5 GiB） |
| max-download | 805306368（768 MiB） |

注意：本机 6/8GB RAM + UFS 2.1。RAM 物理范围需从固件 DTB/内核 dts 实测（picasso 是 8GiB 双 bank，phoenix 亦为双 bank 布局的可能性大，待验证）。

## 1. 技术路线：三层栈（UniXNU 架构）

```
fastboot boot boot.img → BootShim + UEFI(Mu-Silicium, uefi-phoenix)
  → UniXNU.efi (加载驱动/ACPI/DT) → DarwinBoot (boot_args + ADT + trust cache)
  → XNU 内核 (patched, prelink) → iOS/iPadOS 用户态 (IPSW → userdata)
```

非 A/B 设备：走 UniXNU 的 `dual-system-partition` 流程（重分区或写 userdata），或先用 sweet 的"只改 userdata"方案（清数据但保分区表）。

## 2. 三个参考仓库（已克隆到工作区）

| 仓库 | 复用价值 |
| --- | --- |
| `UniXNU-picasso-main`（YinCQL，K30 5G / SM7250） | **最重要**。已达成：锁屏渲染、CDC USB 控制台、UFS root、A620 NativeAdreno 雏形、PMGR、trust cache/AMFI 修复链。同族 Kryo/GIC/PSCI，`make-picasso-adt.py`、prelink 脚本、USB 调试链路全部可改造 |
| `UniXNU-sweet-main`（CrackerCat，Note 10 Pro / SM7150） | **SoC 同平台**。A618 legacy GMU/HFI 传输层（phoenix 直接复用！）、SM7150 ADT（GIC 地址、timer INTID 19、19.2MHz 计数器、PSCI SMC、双 DDR bank）、Darwin23 boot_args ABI、内存 bank 边界审计 |
| `unixnu`（主仓库） | examples/device 模板（format 2）、components.toml、全部文档 docs/porting/* |

## 3. SM7250 → SM7150 移植差距（关键差异点）

1. **GPU：A620 → A618**。A618 用 legacy GMU/HFI（sweet 的 `kits/AdrenoFirmware` 已实现 A618 队列/包长/index），picasso 的 NativeAdreno 骨架 + sweet 的 A618 常量组合即可起步。
2. **CPU 拓扑**：SM7250 是 1+1+6（Kryo 576），SM7150 是 2+6（Kryo 470）。`cpu_regs`/CPU affinity、`cpus=8` 参数与 ADT cpu0..7 节点要按 730G 的 2×A76 修改。
3. **GIC/Timer**：sweet 已给出 SM7150 实测值（INTID 19、Affinity1 编号），直接采用。
4. **面板/触控**：phoenix 为 6.67" IPS LCD（1080×2400）。需从 phoenix 内核 dts 抓取 DSI 时序与触控 IC（Goodix 系列，内核 dts 待确认）。参考 picasso 的 NativeLegacyDisplay 子类（同为 LCD 机型）。
5. **PMIC/PMGR**：SM7150 与 SM7250 同为 QTI Gen3 平台，watchdog/PMGR 常量需按 7150 地址表核对。
6. **boot.img 布局**：phoenix 非 A/B、header v2。picasso 的 `[bootimg.image]`（pagesize 4096、header v1）需改为 v2 并适配 dtb 打包。

## 4. 分阶段计划

### 阶段 A — 设备仓库骨架（1-2 周）
- `cp -R examples/device ../UniXNU-phoenix`，改 `board.toml` / `feeds.toml`（SoC 指向 sweet 的 SM7150 输入 + picasso 的板级组件）
- 从实测数据填 `[userdata]`：`last_lba = 0x1AC55FB000/4096 - 1 = 0x6B159FE`
- OS profile 建议 `ios-16.7.16-20H392`（sweet 验证过 SM7150 + iPhone X 用户态）或对齐 picasso 用 iPadOS 17.7.11
- 验收：`./unixnu defconfig phoenix_defconfig && ./unixnu fetch && ./unixnu status` 通过

### 阶段 B — ADT + 固件（2-4 周）
- 以 `make-sweet-adt.py` 为底做 `make-phoenix-adt.py`（GIC/timer/PSCI 沿用 sweet 值，RAM bank 从 UEFI 内存图实测）
- uefi-phoenix：Mu-Silicium 平台包 `Platforms/Xiaomi/phoenixPkg`（查 Silicium 社区是否已有 sm7150 端口可 fork）
- 验收：`fastboot boot` UEFI 能亮出 UniXNU 菜单（屏幕或 CDC）

### 阶段 C — 内核到控制台（2-4 周）
- 抄 picasso 的 bootstrap defconfig（corecrypto + ArmDTPlatform + ArmCpu）
- USB CDC 控制台（picasso `picasso_usb=2` 全套参数 + 看门狗自动回 fastboot，安全）
- 验收：XNU 内核日志经 USB 串口输出到 Mac

### 阶段 D — 存储 + 用户态（4 周+）
- NativeUfs（SM7150 GCC/PHY 窗口按 7150 地址表改）+ NativeRamBlock + APFS seed
- prelink 链、trust cache、`sheng-lite=1` 等绕障参数照抄 picasso
- 验收：launchd → lock screen 渲染

### 阶段 E — GPU/触控（长期）
- A618 HFI（sweet）+ NativeAdreno 骨架（picasso）合并 → Metal
- Goodix 触控 → NativeHidInput 虚拟化路线先行

## 5. 安全红线（每次上机前检查）

- 先只 `fastboot boot`（RAM 启动），绝不 `fastboot flash` 任何分区直到阶段 D 稳定
- 阶段 D 改 userdata 前必须完整备份（`fastboot` 拉备份不可靠，用 TWRP dd / UFS 全量）
- panic 路径走 PSHOLD + watchdog 自动回 fastboot（picasso 的 `picasso_usb_reset=watchdog` 方案），避免真砖
- anti-rollback=2：不要刷任何更低版本的 aboot/固件

## 6. 深度调研结果（2026-11-06 第二轮）

### 6.1 内核 DTS 实测参数（MiCode phoenix-r-oss，存于 `phoenix-dts/`）

| 子系统 | 参数 |
| --- | --- |
| SoC 标识 | `qcom,msm-id = <365 0x0>`（SDMMAGPIE），`board-id = <43 0>`（QRD） |
| CPU | cpu@0..cpu@700 共 8 核（reg 0x0/0x100/…/0x700），enable-method psci |
| PSCI | `arm,psci-1.0`，method = **smc** |
| GIC | **GICv3**：GICD `0x17a00000`(0x10000)，GICR `0x17a60000`(0x100000，stride 0x20000)，PPI9 |
| PDC | `0xb220000`(0x400)，`qcom,pdc-sdmmagpie` |
| Timer | arch timer **19.2MHz**，PPI 1/2/3/0；memory timer @`0x17c20000` |
| 面板 | **G7B 37-02-0a DSC video**（另有 0b/42-02-0b 变体）：1080×2400、24bpp，H 12/40/40，V 2/30/32，DSC slice-height 20 |
| 触控 | **Novatek NT36xxx（SPI）**：`NVT-ts-spi`@5MHz，reset=tlmm8，irq=tlmm9（不是 Goodix！） |
| UFS | `qcom,ufs-phy-qmp-v3` |
| 电池 | 4500mAh，G7BNVTBM4P / G7BSWDBM4P 两版 |

### 6.2 Mu-Silicium 上游已有 phoenixPkg ✅（存于 `mu-silicium-phoenix/`）

`Project-Silicium/Mu-Silicium` → `Platforms/Xiaomi/phoenixPkg/`（phoenix.dsc/.fdf/.dec、DeviceBuild.py、ConfigurationMapLib、MemoryMapLib）+ `Resources/Configs/phoenix.toml`。**阶段 B 的 UEFI 平台包主体可 forks 即用**。

关键配置（phoenix.toml）：
- boot shim：`requires_kernel_header = true`，boot image **header_version = 1**，kernel gzip + append_dtb
- UEFI FD：`0x9FC00000`，大小 0x300000

MemoryMapLib 已含完整 SM7150 保留区表（DDR 起 `0x80000000`）：HYP/TZApps/QTEE/PIL_REGION/SMEM/Display Reserved(0x9C000000,0x1800000)/UEFI FD 等全部地址——**ADT 的内存与 carve-out 输入直接可用**。

⚠️ 注意：`Resources/DTBs/phoenix.dts` 的 model 是 "APQ 8016 SBC"（DragonBoard 模板残留），**该 DTB 是占位品**；真实固件 DTB 需从设备提取（DT2ARM 或 UEFI 启动时从 ABL 透传），或由真 DTB（内核 phoenix-sdmmagpie.dts 编译产物）替代。

### 6.3 修订后的阶段计划（较原版大幅缩短）

- **阶段 B 缩水**：uefi-phoenix 不再从零做 Mu-Silicium 移植——fork 上游 phoenixPkg，工作变为"编译 + GOP/触控驱动层补齐 + UniXNU 集成（UniXNURamDiskDxe/DiskIoDxe/FAT + 日志导出）"
- **阶段 C 可直接试 boot**：上游已支持 `fastboot boot` 通路（boot shim + header v1 + gzip），风险为零
- ADT 生成：内存 bank 从 MemoryMapLib 取，GIC/PSCI/Timer 取 6.1 实测值（与 sweet 的 SM7150 ADT 交叉验证）

## 7. 当前状态与下一步（2026-11-07 更新）

**已完成，且全部在真机验证过**

- [x] 上游 `phoenixPkg` 在 macOS 上编译通过，产出 `fastboot boot` 可用的 boot.img
- [x] 真机 RAM 启动后出 UniXNU 菜单（Apple logo / `v0.1.0`），GOP 显示正常
- [x] boot image 在 gzip 内核后追加设备树（缺这一步 SEC 死在 MMU 初始化）
- [x] `make-phoenix-adt.py` / `check-phoenix-adt.py` / `tests/test-adt.py`（6 用例，3 负向）
- [x] 载荷卷：4 MiB FAT 压缩进固件 FV（FFS FREEFORM + Tiano 压缩段），运行时
      `ReadSection` 解压 → BlockIo → `ConnectController` → FatPkg → SimpleFileSystem
- [x] `ADTInspector` 从载荷卷读回 `\EFI\UniXNU\phoenix.adt`（12,368 B），6 项检查全 PASS

**下一步**

1. 取真实 DRAM 上界（`adb shell cat /proc/meminfo`），用 `--dram-size` 重新生成 ADT
   （当前是保守值 `0xFCC00000`）。
2. XNU 内核本体：本地 `unixnu/` 构建系统 + `UniXNU-sweet-main` 模板（同为 SM7150），
   XNU 10063.141.1、`ARM64_LINK_ADDR=0xfffffff007004000`，prelink 需要与本机 iOS 匹配的 kernelcache。
3. 阶段 C 的口径按现实收紧：能打出 early boot 日志即算跨过内核入口；SpringBoard 依赖 Metal，
   Adreno 618 上没有可行路径，不作为目标。
