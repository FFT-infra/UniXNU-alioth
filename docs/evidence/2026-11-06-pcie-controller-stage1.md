# PCIe 控制器的只读基线：启动后仍是折叠状态

2026-11-06 傍晚。设备仓库 `NEW/UniXNU-alioth`。真机读数，USB 串口抓取，`fastboot boot`，
不写槽。命令：

```
TAG=p2 ADDON=gpu-sy9.gpuaddon sh tmp/build-fast.sh --kernel-arg pcie=1
sh tmp/rounds-count2.sh tmp/boot-p2.img p2 1 200
```

原始抓取：`tmp/console-p21721591.txt`。

## 1. 这一轮做了什么

`alioth_pcie.c` 的 stage 1，**只读**：不做任何时钟、电源或复位的写操作。目的是确认探针管道
可用，并记录启动后控制器被 bootloader 留在什么状态。

行前缀是 `PCIE 1:`，走 `qcom_usb_debug_write`，串口可见。

## 2. 读数

```
gcc pcie_0_gdscr       0x22f001
gcc pcie_0_axi_misc    0x04100040
gcc pcie_0_slv_axi_halt 0x80000220
gcc pcie_0_mstr_axi_halt 0x80000220
gcc pcie_0_cfg_ahb_halt 0x80000000
gcc pcie_0_aux_halt     0x80000000
gcc pcie_0_pipe_halt    0x80000000
gcc pcie_0_slv_q2a_halt 0x80000000
```

- `pcie_0_gdscr = 0x22f001` 与 2026-10-28 那轮（`docs/evidence/2026-10-28-gpu-wifi-bringup.md`）
  读到的值一致，是控制器完全折叠时的取值。
- 四个 halt 寄存器都只有 bit 31 与各自的 acknowledge 位：**控制器被按在复位里**，不是"已经
  上电但链路没起来"。
- `slv_axi_halt` / `mstr_axi_halt` 的 `0x220` 是 AXI 两侧的 ack，`cfg_ahb`/`aux`/`pipe`/`slv_q2a`
  是 `0x80000000`，即只有 halt 位、没有 ack。

这也说明 `porting/network` 要求的顺序是对的：从这里开始才是"上电、给时钟、按复位序列、训练链路"。

## 3. 对桌面的影响

无。同一轮 `sw=71/71/0`、`own=1`、`tf` 正常增长。只读探针不进显示路径。

## 边界

- 只验证了两件事：探针在到达 stage 1 时能跑，且控制器处于折叠状态。
- stage 10 到 14（`WLAN_EN`、`PERST#`、`PARF_LTSSM` bit 8、七个 dm_core 写、ELBI 读）**没有
  跑过**。它们要等组件层就位才有意义：按 `porting/network`，Wi-Fi 需要 `Ath12kWifi` 组件、
  `lx_platform`、`nw_board` 与 `WifiNet`，本仓库三样都没有，且组件描述的端点（WCN7850 /
  QCC2072 / QCN9274）不包含本板的 QCA6390。
- 固件也不在工作区：`proprietary-files.txt` 没有 WLAN 条目，也没有 vendor 镜像或 NON-HLOS.bin。
