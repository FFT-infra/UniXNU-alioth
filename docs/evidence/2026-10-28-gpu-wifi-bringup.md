# GPU 与 Wi-Fi 的真机读数（2026-10-28）

本文件是 `porting.toml` 中 gpu 与 network 两个 area 的 evidence 指向。

## GPU（Adreno 650）

`pipa-gpu=1`，boot-usb106：

```
GPUROW0 GPU m1 gcc=ok cc=ok smmu=skip gpu=skip io=0 cx0 clocks0 ahb0
```

`pipa-gpu=6`，boot-usb107：

```
GPUROW0 GPU m6 gcc=ok cc=ok smmu=ok gpu=skip io=0 cx1 clocks1 ahb0
```

`pipa-gpu=7`，boot-usb122（附写测试）：

```
GPUROW13 GMUCTX step 4 status e00002ca kit 0/10 routes 0 id2 5511 phase 5 wr 13 rb 1 en 1
GPUROW14 PRE idr0 4c017e06 smr5 8000007f->00000000 scr0 0026040e->00260406
```

结论：CX 域可上电、SMMU/GMU 时钟可运行；GMU 上下文在 Attach 步失败，因为**这颗 SMMU
对 EL1 的写全部被接受且全部不生效**（SMR 与 sCR0 两次写入均用了不同值，均读回原值），
而同一映射的读正确、同构造的 GPUCC 窗口写生效。

## Wi-Fi（QCA6390）

`pcie=1`，boot-usb129：

```
PCIE 1: gcc pcie_0_gdscr (0xfffffff84986b004) = 0x22f001
```

`pcie=2`，boot-usb130：

```
PCIE 2: about to write gcc pcie_0_gdscr ...: 0x22f000
PCIE 2: gcc pcie_0_gdscr after (...) = 0xf822f000
PCIE 2: THE CONTROLLER HAS POWER (gdscr 0xf822f000, 1 ms)
```

`pcie=3`，boot-usb131：`aux_halt = 0`、`slv_q2a_halt = 0`。

`pcie=4`，boot-usb132：`phy_status = 0x68`，bit 6 置位，未写 PHY 即就绪。

`pcie=9 pcie_skip=32`，boot-usb133：

```
PCIE 9: parf_mhi_clock_reset_ctrl = 0x3 -> 0x13
PCIE 9: parf_axi_mstr_wr_addr_halt_v2 = 0x80000014
```

结论：控制器可从完全塌陷上电并经 PARF 配置，全部与 ios-sweet 系列在这台机器上的记录一致。
step 10（DBI/配置空间/链路训练）在参考实现里也从未运行。
