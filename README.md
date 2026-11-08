# 归档：phoenix-unixnu 收尾时只存在于本机的东西

2026-11-08 项目暂停，本地工作区（5.5 GiB）已清理。这个分支只保留四样
**无法从上游重建、也无法从 GitHub 找回**的内容，方便日后原样恢复当时的状态。

主分支 `main` 是干净的源码仓库；这个分支是历史快照，**不要合并进 main**。

## 内容

| 文件 | 大小 | 来源 | 为什么不可重建 |
| --- | --- | --- | --- |
| `phoenix-debug-kernel` | 1,422,952 B | 从手机原厂 boot image 的 kernel 段提取 | 需要一台还在跑的 phoenix 才能重新 dump |
| `phoenix-trimmed.dtb` | 397,929 B | 从原厂 boot image 的 FDT 段提取（`d00dfeed` 切分） | 同上。注意它和 `main` 里的 `dtb/phoenix-initrd.dtb`（398,781 B）不是同一份 |
| `probes-mu-basecore.patch` | 3,545 B / 115 行 | `Mu_Basecore` 工作树里 P0–P5 调试打印的 diff | 从未提交，只活在当时的工作树里 |
| `memory/` | 5 篇 | WorkBuddy 的项目记忆笔记（定义、状态、命令、坑） | 无云端副本 |

`probes-mu-basecore.patch` 里全是 `DEBUG((DEBUG_ERROR, "P0\n"))` 这类噪音打印，
**没有功能改动**。`main` 的 `patches/0002` 故意排除了它们，所以不需要它也能编译出
同样的固件——留着只是因为它是那几天排黑的唯一记录。

## 怎么用

```sh
git clone https://github.com/HarmonyHI/phoenix-unixnu.git
cd phoenix-unixnu
git checkout archive/2026-11   # 就是这个分支

# 想恢复记忆笔记（WorkBuddy 项目记忆）
cp -R memory ~/某个工作区的/.workbuddy/

# 想重新加回调试打印
git -C /path/to/Mu-Silicium/Mu_Basecore apply probes-mu-basecore.patch

# 想用原厂 DTB 重新做一个 boot image
#   phoenix-trimmed.dtb 就是 dtb/README.md 里说的「切 d00dfeed」的产物
```

## 来源与许可

`phoenix-debug-kernel`、`phoenix-trimmed.dtb` 派生自小米 Redmi K30 4G 的原厂
boot image，其内核部分以 GPL-2.0 发布。这里只作为本人设备的移植研究存档，
不构成再分发许可；如需引用请自行从小米官方开源发布获取原始材料。

其余文件（patch、笔记）为本项目产出，随 `main` 的许可。
