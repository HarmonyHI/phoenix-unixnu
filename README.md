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
| FAT 载荷卷 | ✅ 已内嵌进固件 FV（FFS FREEFORM + Tiano 压缩段），运行时 `ReadSection` 解出 4 MiB 并验证 `size 4194304` |
| FAT 镜像合法性 | ✅ `fsck_msdos` 干净；`checkfat.py` 按 FatPkg 语义逐项复核（几何、FAT 类型、双 FAT、LFN 校验和与顺序、簇链、大小写不敏感路径） |
| BlockIo → DiskIo → SimpleFileSystem 链路 | ✅ 已上机验证：`Installed Fat fileSystem` → `ConnectController Success` → `1 SimpleFileSystem handle(s)` |
| 从 OS 内部读回 `\EFI\UniXNU\phoenix.adt` | ✅ **已上机确认**（2026-11-07，`Mu-phoenix-fv6.img`）：`config FOUND`（unixnu.toml）+ ADTInspector 6 项全过 |
| ADT 生成器 / 校验器 / 回归测试 | ✅ 生成 12 KiB ADT；校验器 8 条不变量；`tests/test-adt.py` 6/6 通过（含 3 个负向用例） |
| 真实 DRAM 上界 | ⚠️ 暂用保守值 `0xFCC00000`，需 `adb shell cat /proc/meminfo` 拿真值后重新生成 |
| XNU 内核本体 | ❌ 未构建：需要 Xcode 26.2、Apple 源码投放、以及与本机 iOS 版本匹配的 kernelcache 做 prelink |
| 显示 / 触控 / UFS / GPU | ❌ 未开始 |

上机实测输出（2026-11-07，`Mu-phoenix-fv6.img`）：

```
UNIXNU-ADT: phoenix ADT inspector
UNIXNU-ADT: read 12368 bytes from \EFI\UniXNU\phoenix.adt
UNIXNU-ADT: check file PASS (phoenix.adt present on the payload volume)
UNIXNU-ADT: check parse PASS (12368 bytes consumed of 12368)
UNIXNU-ADT: check model PASS (model 'phoenix-platform-bringup')
UNIXNU-ADT: check dram PASS (dram 100000000 + FCC00000 (owned 40000000))
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
  0001-mu-silicium-phoenix-port.patch       对已跟踪文件的改动（dsc/fdf/APRIORI/MemoryMapLib/BootManager 等）
  0002-mu-basecore-flatten-pdbs-and-dump.patch  Mu_Basecore 的 12 个文件（详见下方说明）
dts/                          MiCode phoenix-r-oss 设备树文本（SoC / 面板 / 触控 / pinctrl）
dtb/phoenix-initrd.dtb        随镜像下发的设备树（唯一不可重建的输入，见 dtb/README.md）
scripts/apply-to-mu-silicium.sh 把 overlay + 设备树 + 补丁落到干净的上游树
```

## 从克隆到菜单

克隆本仓库**本身还不能开机** —— 它只是一个"叠加层"，底下需要上游固件树、一份设备树、
和一套能编 EDK2 的工具链。完整步骤如下，按顺序做能一次跑到 UniXNU 菜单。

前置：macOS + Xcode CLT、Python 3、`fastboot`、Homebrew 的 LLVM 18 与 lld
（`brew install llvm@18 lld`）、约 6 GiB 磁盘、能访问 GitHub（上游含 submodule）。

```sh
# 1. 上游固件树（本项目基于此提交），连同 Mu_Basecore 子模块约 5 GiB（含各自 .git）
git clone https://github.com/Project-Silicium/Mu-Silicium.git mu-silicium
git -C mu-silicium checkout 8471e47f5248ebbffc2e6196b683204163efe155
git -C mu-silicium submodule update --init --recursive   # Mu_Basecore @ 64f4789

# 2. 本仓库放在 firmware 旁边（apply 脚本用相对路径找它）
git clone https://github.com/HarmonyHI/phoenix-unixnu.git
./phoenix-unixnu/scripts/apply-to-mu-silicium.sh mu-silicium
#    └─ 铺 overlay 新文件（UniXNURamDiskDxe / PayloadTest / 脚本）
#    └─ 放设备树 dtb/phoenix-initrd.dtb → mu-silicium/phoenix-initrd.dtb
#    └─ 打 patches/0001（dsc / fdf / APRIORI / MemoryMapLib / BootManager）

# 3. 构建：pass1 出 EFI → 生成 ADT → 打成 FAT → pass2 把 FAT 压进固件 FV → mkbootimg
cd mu-silicium && ./Resources/Scripts/build-phoenix.sh Mu-phoenix-menu.img
#    约 1 分钟；末尾会打印 payload / ADT 校验 / checkfat / dtb 追加情况

# 4. 只 RAM 启动，不写任何分区
fastboot reboot-bootloader && sleep 6
fastboot boot Mu-phoenix-menu.img
```

屏幕应当出现 Apple logo、`version 0.1.0`、`config: \EFI\UniXNU\unixnu.toml`，以及四个条目
（`phoenix ADT inspector` / `Hello payload` / `XNU kernel` / `Return to firmware`）。
选第一项，会打印 8 行 `UNIXNU-ADT:` 并以 `RESULT PASS 6 passed 0 failed` 结尾。

几个会挡路的细节：

- `patches/0002`（Mu_Basecore 侧）**不自动应用**，apply 脚本只打 0001；要精简异常 dump 等
  改动时手动 `git -C mu-silicium/Mu_Basecore apply`。不应用也能构建，只是 dump 更长。
- `phoenix-initrd.dtb` 是唯一不可重建的输入（`dts/` 里的文件不完整，`sm7150.dtsi` 为空），
  所以随仓库提供，见 `dtb/README.md`。
- 打包脚本对 DTB 是硬检查；缺了不会静默产出镜像。缺 DTB 的症状是只打一行
  `splitting block entry with MMU disabled` 就复位 —— 别误判成驱动问题。
- `fastboot boot` 连续报 `Failed to load/authenticate boot image: Load Error` 时，
  先 `fastboot reboot-bootloader` 再来；同一个 bootloader 会话里连收大镜像会被 ABL 拒绝。
- `fastboot devices` 为空**不等于**固件在跑：也可能已经复位进 Android 了。

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
| `phoenix-trimmed.dtb`、`phoenix-debug-kernel`、`out/phoenix.adt` | 原厂 kernel 镜像的提取物与中间产物；真正要用的设备树已固化进 `dtb/` |
| `unixnu/` 上游源码树（24 MB） | 有独立上游，克隆即得；本仓库只留对它的移植结论 |
| `UniXNU-sweet-main/`、`UniXNU-picasso-main/` | 第三方端口参考，不属于本次工作 |
| Mu_Basecore 里 6 处临时探针（`P0`–`P5`、`GSA … ra=…`） | 黑屏排查时插的噪音打印，只在 `ArmMmuLibCore.c` / `CpuMmuCommon.c` / `Gcd.c`，不进补丁 |
| `BaseTools/Bin/{basetoolsbin,nasm}_ext_dep.yaml` 的删除 | 本机改用已装工具的副作用；别人的 `stuart update` 还要靠它拉 basetools |

所以 `patches/0002-mu-basecore-flatten-pdbs-and-dump.patch` 只保留 12 个文件的实质改动：
FlattenPdbs 跳过开关、精简 AArch64 异常 dump（去 V 寄存器、栈窗口 16→4 行，让 PC 模块符号化留在屏幕内）、
timer 频率的 PCD 回退、BootManagerMenu 的 Power Off / Exit 与配色、UsbBus 的 TPL 调整、
AuthVariable 运行时写保护、ShellLib 构造期 `EFI_NOT_FOUND` 容忍。补丁用 `git apply --check -R`
对当前工作树反向验证过，与工作树逐字节一致。

## 踩过的坑（摘要，细节见 `docs/notes-infeasible-routes.md`）

1. **`/chosen` 里没有 `linux,initrd-start`** —— ABL 给的 blob 只有 `bootargs`，"靠设备树拿 ramdisk"这条路在本机型上不存在。
2. **在 SEC 里抢救 `x0`（ABL 传的 DTB 指针）失败** —— 重定位验证正确，但实测两次全黑屏、一个字都没有，已回退。现改为在内存里扫 `UNIXNU00` 签名 + 回退到 FV 内嵌副本。
3. **全 DRAM 扫签名会撞未映射空洞**（`0x84920000-0x85700000`、`0x98500000-0x9C000000`）→ data abort 重启。扫描必须限定在已映射区间。
4. **`UFSDxe` / `PILDxe` 必须从 phoenix 的 APRIORI/DXE 列表去掉** —— 前者初始化时挂死看门狗，后者读不到 `uefipil.cfg` 时跳空指针。
5. **UEFI Shell 不能当载荷** —— `ShellLibConstructor` 在任何协议安装前就 assert。专用小程序更小更可控。
6. **FDT 二进制解析容易错位** —— token 常量实际是 `BEGIN=1 / END=2 / PROP=3 / NOP=4 / END_NODE=9`，节点名为空时必须按 4 字节对齐。
7. **`mkfat.py` 三个隐蔽 bug** —— 没调用 LFN 写入（`unixnu.toml` 变成 `UNIXNU.TOM`）、FAT[0] 写成 FAT12 的 `0xF8FF`、`--add DIR=HOST` 的斜杠被当文件名。已重写并配 `checkfat.py`。
8. **boot image 必须在 gzip 内核后追加 DTB** —— 打包脚本丢了这一步后，SEC 拿不到设备树，症状是孤零零一行 `splitting block entry with MMU disabled` 然后复位回系统。`mkinitrd.sh` 现在硬检查 DTB。
9. **`EFI_FIRMWARE_VOLUME2_PROTOCOL.GetNextFile` 的 `FileType` 是输入过滤** —— 不初始化为 0（`EFI_FV_FILETYPE_ALL`）就按栈垃圾过滤，循环一次就 `EFI_NOT_FOUND`，表现为"固件里没有载荷"。
10. **`GetNextFile` 的 `Key` 必须是 `UINTN` 大小** —— 实现按 8 字节读写，传 `UINT32` 会让高 4 字节栈垃圾被当链表指针解引用（data abort / translation fault，`FAR=0`）。
11. **BDS 阶段扫 DRAM 会挂死总线** —— `0x85700000-0x86000000`（HYP）与 `0x86200000-0x98500000`（QTEE/PIL）读一下就静默冻屏（连 data abort 都不来）。所有早期诊断只碰已映射区间，或者干脆别扫。

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

1. ~~上机跑 ADT 校验~~ ✅ 已完成（2026-11-07，6 passed / 0 failed）。
2. 拿到真实 DRAM 上界（`/proc/meminfo`），用 `--dram-size` 重新生成 ADT。
3. 然后才是内核：Xcode 26.2 + Apple 源码投放 + 匹配本机的 `kernelcache` 做 prelink（`ARM64_LINK_ADDR=0xfffffff007004000`）。

## 上游与致谢

- [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) —— EDK2 分支，`Platforms/Xiaomi/phoenixPkg` 提供了本机 UEFI 平台包
- [UniXNU](https://github.com/UniXNU) 上游 —— 设备树模板、`kits/`、移植文档
- `UniXNU-sweet-main`（CrackerCat，SM7150 同平台）与 `UniXNU-picasso-main`（YinCQL，SM7250）—— ADT 常量、USB CDC 控制台、watchdog 回退方案的主要参考
- MiCode `phoenix-r-oss` —— 本仓库 `dts/` 的来源
