# phoenix-unixnu

在 **Redmi K30 4G**（`phoenix`，Qualcomm SM7150 / 骁龙 730G，Adreno 618）上，用
Mu-Silicium UEFI + UniXNU 引导 XNU 内核的移植工作区。

设备 bootloader 已解锁，但整个项目只使用 `fastboot boot`（RAM 启动），**不写任何分区**。

```
fastboot boot boot.img
  └─ BootShim + Mu-Silicium (uefi-phoenix, FD @ 0x9FC00000)
      └─ UniXNU.efi ── 载入内嵌 FAT 载荷卷 → 菜单（Apple logo / v0.1.0）
          └─ Payloads/ADTInspector.efi ── 读 \EFI\UniXNU\phoenix.adt 并校验
              └─ SpringBoard 之前的最后一块空地：真正的 XNU 内核（尚未构建）
```

这个仓库只存**源码、补丁、脚本、文档**。固件树（Mu-Silicium）、XNU 源码树（unixnu）、
任何 `.img` / `.fd` / `.efi` / `.fat` / `.dtb` 都不在这里 —— 它们要么能克隆，要么能重新构建。

## 当前进度（2026-11-07）

| 项目 | 状态 |
| --- | --- |
| UEFI 固件在真机启动 | ✅ 已上机：`fastboot boot` 后出 UniXNU 菜单（Apple logo、`v0.1.0`），有照片留证 |
| FAT 载荷卷 | ✅ 已内嵌进固件 FV（FFS FREEFORM + Tiano 压缩段），绕开 ABL 是否放置 ramdisk 的不确定性 |
| FAT 镜像合法性 | ✅ `fsck_msdos` 干净；`checkfat.py` 按 FatPkg 语义逐项复核（几何、FAT 类型、双 FAT、LFN 校验和与顺序、簇链、大小写不敏感路径） |
| BlockIo → DiskIo → SimpleFileSystem 链路 | ✅ 已实现；BlockIo 发布后主动 `ConnectController` 才会挂上 FAT |
| 从 OS 内部读回 `\EFI\UniXNU\phoenix.adt` | ⏳ **待上机确认**（`Mu-phoenix-adt.img` 已构建，5,302,272 B） |
| ADT 生成器 / 校验器 / 回归测试 | ✅ 生成 12 KiB ADT；校验器 8 条不变量；`tests/test-adt.py` 6/6 通过（含 3 个负向用例） |
| 真实 DRAM 上界 | ⚠️ 暂用保守值 `0xFCC00000`，需 `adb shell cat /proc/meminfo` 拿真值后重新生成 |
| XNU 内核本体 | ❌ 未构建：需要 Xcode 26.2、Apple 源码投放、以及与本机 iOS 版本匹配的 kernelcache 做 prelink |
| 显示 / 触控 / UFS / GPU | ❌ 未开始 |

期望的上机输出（`ADTInspector`）：

```
UNIXNU-ADT: read 12288 bytes from \EFI\UniXNU\phoenix.adt
UNIXNU-ADT: check file PASS (...)
UNIXNU-ADT: check parse PASS (...)
UNIXNU-ADT: check model PASS (model 'phoenix-platform-bringup')
UNIXNU-ADT: check dram PASS (dram 100000000 + fcc00000 (owned 40000000))
UNIXNU-ADT: check product PASS (product 'Redmi K30 4G')
UNIXNU-ADT: check cpus PASS (8 cpu nodes)
UNIXNU-ADT: RESULT PASS 6 passed 0 failed
```

这一屏同时证明：载荷卷挂载成功、FAT 驱动能解析长文件名、文件被完整读出、生成的 ADT 结构自洽。

## 仓库结构

```
docs/
  PORTING-PHOENIX.md          总路线图：调研结论、差距分析、五个阶段、安全红线
  hardware-facts.md           每个硬件常量的取值与证据来源
  notes-infeasible-routes.md  五条已经踩死的路线（别重试）+ 下一步
port/                         UniXNU 设备/平台树（board.toml、defconfig、ADT 工具、测试）
  scripts/make-phoenix-adt.py   生成 Apple 设备树（ADT，非 FDT）
  scripts/check-phoenix-adt.py  反向解析并校验 8 条不变量
  tests/test-adt.py             宿主回归测试，6 用例（3 个负向）
  configs/phoenix_defconfig     内核平台 bring-up 配置（从未编译过）
overlay/                      Mu-Silicium 里的**新增**源码，保持原相对路径
  UniXNU/                       UniXNURamDiskDxe（本轮核心）、UniXNULib、DarwinBootPkg…
  Platforms/Xiaomi/phoenixPkg/Application/PayloadTest/
  Resources/Scripts/            mkfat.py / checkfat.py / build-phoenix.sh / mkinitrd.sh
patches/
  0001-mu-silicium-phoenix-port.patch   对已跟踪文件的改动（253 行：dsc/fdf/APRIORI/MemoryMapLib/BootManager）
  0002-mu-basecore-flatten-pdbs-gate.patch  EDK2 插件开关，允许跳过 PDB flatten
dts/                          MiCode phoenix-r-oss 设备树（SoC / 面板 / 触控 / pinctrl）
scripts/apply-to-mu-silicium.sh 把 overlay + 补丁落到干净的上游树
```

## 复现

前置：macOS + Xcode CLT、Python 3、`fastboot`、约 6 GiB 磁盘（固件构建产物）。

```sh
# 1. 上游固件树（本项目基于此提交）
git clone https://github.com/Project-Silicium/Mu-Silicium.git mu-silicium
git -C mu-silicium checkout 8471e47f5248ebbffc2e6196b683204163efe155
git -C mu-silicium submodule update --init --recursive   # Mu_Basecore @ 64f4789
# Mu_Basecore 里另有一处本地改动（BootManagerMenu / UsbBus / Gcd / ArmMmuLib 等），不属于本仓库

# 2. 落地本仓库的内容
git clone https://github.com/HarmonyHI/phoenix-unixnu.git
./phoenix-unixnu/scripts/apply-to-mu-silicium.sh mu-silicium

# 3. 构建：EFI → 打成 FAT → 嵌进固件 FV → mkbootimg
cd mu-silicium && ./Resources/Scripts/build-phoenix.sh Mu-phoenix-adt.img

# 4. 只 RAM 启动，不刷分区
fastboot boot Mu-phoenix-adt.img
```

ADT 工具可以脱离固件单独跑：

```sh
python3 port/scripts/make-phoenix-adt.py --output out/phoenix.adt
python3 port/scripts/check-phoenix-adt.py out/phoenix.adt
python3 port/tests/test-adt.py
# 拿到真实 DRAM 上界后重新生成：
python3 port/scripts/make-phoenix-adt.py --output out/phoenix.adt --dram-size 0x1fcc00000
```

## 刻意排除的东西

| 排除项 | 原因 |
| --- | --- |
| `Build/`（269 MB）、`Binaries/`、`*.fd`、`*.efi` | 可重新构建 |
| `Payload/payload.fat`（4 MB） | `build-phoenix.sh` 每次自动生成 |
| `phoenix-trimmed.dtb`、`phoenix-debug-kernel`、`out/phoenix.adt` | 二进制中间产物 |
| `unixnu/` 上游源码树（24 MB） | 有独立上游，克隆即得；本仓库只留对它的移植结论 |
| `UniXNU-sweet-main/`、`UniXNU-picasso-main/` | 第三方端口参考，不属于本次工作 |

## 踩过的坑（摘要，细节见 `docs/notes-infeasible-routes.md`）

1. **`/chosen` 里没有 `linux,initrd-start`** —— ABL 给的 blob 只有 `bootargs`，"靠设备树拿 ramdisk"这条路在本机型上不存在。
2. **在 SEC 里抢救 `x0`（ABL 传的 DTB 指针）失败** —— 重定位验证正确，但实测两次全黑屏、一个字都没有，已回退。现改为在内存里扫 `UNIXNU00` 签名 + 回退到 FV 内嵌副本。
3. **全 DRAM 扫签名会撞未映射空洞**（`0x84920000-0x85700000`、`0x98500000-0x9C000000`）→ data abort 重启。扫描必须限定在已映射区间。
4. **`UFSDxe` / `PILDxe` 必须从 phoenix 的 APRIORI/DXE 列表去掉** —— 前者初始化时挂死看门狗，后者读不到 `uefipil.cfg` 时跳空指针。
5. **UEFI Shell 不能当载荷** —— `ShellLibConstructor` 在任何协议安装前就 assert。专用小程序更小更可控。
6. **FDT 二进制解析容易错位** —— token 常量实际是 `BEGIN=1 / END=2 / PROP=3 / NOP=4 / END_NODE=9`，节点名为空时必须按 4 字节对齐。
7. **`mkfat.py` 三个隐蔽 bug** —— 没调用 LFN 写入（`unixnu.toml` 变成 `UNIXNU.TOM`）、FAT[0] 写成 FAT12 的 `0xF8FF`、`--add DIR=HOST` 的斜杠被当文件名。已重写并配 `checkfat.py`。

## 硬件事实速查

| 项 | 值 |
| --- | --- |
| SoC | SM7150（`qcom,msm-id = <365 0x0>`，SDMMAGPIE），8 核 = 2×A76@2.2G + 6×A55@1.8G |
| GPU | Adreno 618（与 sweet / Note 10 Pro 同款） |
| 存储 | UFS，4 KiB 逻辑块；`userdata` = 0x1AC55FB000（≈106.86 GiB） |
| 分区方案 | 非 A/B，有 `super`，无 `vendor_boot` → boot header **v1**（上游 phoenix.toml 配置） |
| UEFI FD | `0x9FC00000`，0x300000 |
| GIC | GICv3：GICD `0x17a00000`、GICR `0x17a60000`（stride 0x20000），PPI9 |
| Timer | arch timer 19.2 MHz，PPI 1/2/3/0；memory timer @ `0x17c20000` |
| PSCI | `arm,psci-1.0`，method = smc |
| 面板 | G7B 37-02-0a DSC video，1080×2400，24bpp |
| 触控 | Novatek NT36xxx（SPI，5 MHz，reset=tlmm8，irq=tlmm9）—— **不是 Goodix** |

## 安全红线

- 只 `fastboot boot`；在阶段 D 稳定前不 `fastboot flash` 任何分区。
- 改 `userdata` 前必须完整备份（`fastboot` 拉备份不可靠，用 TWRP `dd`）。
- panic 走 PSHOLD + watchdog 自动回 fastboot，别让设备停在半死状态。
- `anti-rollback = 2`：不要刷任何更低版本的 aboot / 固件。

## 下一步

1. 上机跑 `Mu-phoenix-adt.img`，拍下 `UNIXNU-ADT:` 那一段 —— 无论 PASS 还是 FAIL，"载荷卷可读"这个问题就此关闭。
2. 拿到真实 DRAM 上界（`/proc/meminfo`），用 `--dram-size` 重新生成 ADT。
3. 然后才是内核：Xcode 26.2 + Apple 源码投放 + 匹配本机的 `kernelcache` 做 prelink（`ARM64_LINK_ADDR=0xfffffff007004000`）。

## 上游与致谢

- [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) —— EDK2 分支，`Platforms/Xiaomi/phoenixPkg` 提供了本机 UEFI 平台包
- [UniXNU](https://github.com/UniXNU) 上游 —— 设备树模板、`kits/`、移植文档
- `UniXNU-sweet-main`（CrackerCat，SM7150 同平台）与 `UniXNU-picasso-main`（YinCQL，SM7250）—— ADT 常量、USB CDC 控制台、watchdog 回退方案的主要参考
- MiCode `phoenix-r-oss` —— 本仓库 `dts/` 的来源
