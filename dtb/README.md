# dtb/ — 必须随 boot image 一起下发的设备树

`phoenix-initrd.dtb`（398,781 字节，FDT v17，`d00dfeed`）是**唯一不能重新构建的输入**，
所以放在仓库里，而不是当产物排除。

## 它是什么

- 来源于本机自己的 ABL/原厂 boot image 里那份设备树（Qualcomm `sdmmagpie` / SM7150，
  带 `qcom,msm-id`、`qcom,board-id`），不是从 `dts/` 编出来的 —— 仓库里的 `dts/` 只是
  MiCode `phoenix-r-oss` 的部分文件，`sm7150.dtsi` 是空文件、include 链不完整，编不出来。
- 同源的 `phoenix-trimmed.dtb`（397,929 字节）就是从原厂 kernel 镜像里按 `d00dfeed`
  切出来的那一段；本文件比它多 852 字节，是早期手工调整后的版本，即实际随镜像下发的那个。
- 内容只保留固件需要的部分（`/memory`、`/cpus`、GIC、`/chosen` 等），显示/存储等节点已裁掉。

## 为什么要它

`mkinitrd.sh` 会把它**追加在 gzip 内核之后**。ABL 把这段 DTB 的地址通过 `x0` 交给固件，
SEC/PEI 靠它建内存映射；缺了它，症状是屏幕只孤零零打一行
`splitting block entry with MMU disabled`，然后复位进 Android —— 而且没有异常 dump，
看上去像"卡住"，很容易误判成驱动问题。

打包脚本对这个文件是硬检查：找不到就直接报错退出，不会产出"能刷但起不来"的镜像。

## 换设备 / 重新提取

从自己的机器上取一份（设备树属于 Linux 内核源码派生物，GPL-2.0，随镜像分发无授权问题）：

```sh
# 方式一：从原厂 boot image 里切（dtb 通常跟在 kernel 后面）
python3 - <<'PY'
d = open('stock-boot.img', 'rb').read()
i = d.find(b'\xd0\x0d\xfe\xed')
size = int.from_bytes(d[i+4:i+8], 'big')
open('phoenix-initrd.dtb', 'wb').write(d[i:i+size])
print('dtb at', i, 'size', size)
PY

# 方式二：设备已启动 Linux/Android 时直接读运行中的 FDT
adb shell cat /sys/firmware/fdt > phoenix-initrd.dtb
```

换成本机的 DTB 后，`dtb/README.md` 里这些数字（大小、`msm-id`、`board-id`）要一起更新，
并重跑一遍 `port/tests/test-adt.py` —— ADT 里的内存与 CPU 描述应与这份 DTB 对得上。
