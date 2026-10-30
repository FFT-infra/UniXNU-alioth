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

## 追加：step 5 通过，step 10 仍未通过

`pcie=5`（不跳过 PHY）：

```
PCIE 5: gcc pcie_phy_aux after the reset (0xfffffff274507004) = 0x1
PCIE 5: gcc pcie_0_phy_bcr after (0xfffffff27450401c) = 0
PCIE 5: about to write gcc pcie_phy_com_bcr (release) at 0xfffffff274507010: 0
PCIE 5: the PHY IS READY (status 0x69, bit 6 is its ready)
PCIE 5: step 5: done
```

ios-sweet 当年跳过这一步（`pcie_skip=32`），因为它释放 PHY common reset 时会把整机打死。
**在本内核与本板上它跑完且 PHY 就绪（0x69）**，所以那条限制不适用于这里。

`pcie=10`（已有 pipe clock）：日志停在 `step 9: done`，设备回到 bootloader。
所以"step 10 需要 pipe clock"这个判断**最多只对了一半**，下一步要读的是它第一个访问碰了什么。

## step 10 是只读的，却把机器打死

`alioth_pcie.c:603` 的 `step_dbi()` 全部是 `get()`（读）：

```
get(4, "iatu_viewport", 0x00);
get(5, "conf_vendor_device (bus 0, device 0, function 0)", 0x00);
get(5, "conf_header_type", 0x0c);
get(6, "dbi_vendor_device", 0x00);
get(6, "dbi_capability", 0x70);
get(3, "elbi_sys_ctrl", 0x04);
```

所以在有 pipe clock 的情况下它仍然复位，**不是写坏了什么，而是第一次访问就挂了**——
按顺序是窗口 4 的 `iatu_viewport`（`0x60001000`）。

这条把下一步缩小到一个具体动作：**单独读窗口 4 的 0x00**，其它都不碰。
如果那一读就挂，问题在 IATU 窗口的映射或它的访问许可；如果读得回来，再往后一个一个试。

## 触控：每一轮的真机读数（2026-10-28）

| 轮次 | 配置 | 关键读数 | 结论 |
| --- | --- | --- | --- |
| 起初 | 单一 HID 设备 | `hres=1`、`hid` 缓慢增长、界面不动 | 报告被拒（当时的判断） |
| 分离设备 | `sheng-touch-split=1` | `tsplit=1` | 设备起来了 |
| 分离 + 触摸 | 同上 | `low=190`、`raw=000042e10607`、`[touch multi] lifts=14` | **触控核心正常，14 次接触都完成** |
| 判据更正 | — | 分离前 `edge[1]` 曾到 16/23 | `hres=1` **不是拒绝**，合并设备返回它是预期 |
| edge 修复前 | 全局 `originalDispatch` | `edge=1,0`、`[touch multi] reports=0` | 分离设备的驱动从未被包，`FromEdgeTip` 不注入 |
| edge 修复后 | per-driver（子模块 `1eade81`） | `reports 0→781`、`max=5`、`lifts=88` | 事件进系统了；但 **5 个同时触点是"手掌"** |
| area 过滤 + 关 split | `tsplit=0` | `irq=1`、`low=0`、`raw=ffffffffffff` | 抓取窗口内芯片无数据（空闲态） |

**`low` 不是中断计数**：源码里它是轮询时看到 GPIO 39 为低的次数；`irq=1` 是当前电平（高=空闲）。
`int=01` 是芯片内部计数器 0x8F 的残留值，与 `low` 不矛盾。

**下一步的判据**：在确认手指按住屏幕的窗口里读同一行。
`low` 上升 → 芯片在报事件，问题在 HID 语义（`max` 是否仍 >1、area 过滤是否把它们全滤掉）；
`low` 恒为 0 → 芯片没有扫描，问题在触控 IC 侧，与 HID 无关。

## 2026-10-30：step 10 复现，并把范围缩到时钟投票

`pcie=10`（十步全开、不跳过任何一步）上板后**板子再没回来**：无 fastboot、无 adb、
`/dev/tty.usbmodem14101` 不枚举。上一次同样的尝试记录是「日志停在 `step 9: done`，设备回到
bootloader」，这次连 bootloader 都没到。同一动作两种结果，说明这条路径现在每次尝试都可能要断电，
且不一定换得到信息。参数已撤回。注意 `pcie=10` 是记录里早就写好的下一步（`step_dbi` 只做一次
`get(4, "iatu_viewport", 0x00)`），不是新试验。

**窗口地址与 vendor 完全一致，不是映射错。** 对照 `reference/k40-kernel/arch/arm64/boot/dts/vendor/qcom/kona-pcie.dtsi:7-13`：

| reg-name | vendor | 我们 |
| --- | --- | --- |
| parf | `0x01c00000 0x3000` | 同 |
| phy | `0x01c06000 0x1000` | 同 |
| dm_core | `0x60000000 0xf1d` | 同 |
| elbi | `0x60000f20 0xa8` | 同 |
| iatu | `0x60001000 0x1000` | 同 |
| conf | `0x60100000 **0x100000**` | `0x1000` ← 唯一差异，且不是挂死的那次访问 |

**真正可疑的是时钟投票集合。** vendor 的 `clock-names`（同文件 `:76-81`）列了十一项：

```
pcie_0_pipe_clk, pcie_0_ref_clk_src, pcie_0_aux_clk, pcie_0_cfg_ahb_clk,
pcie_0_mstr_axi_clk, pcie_0_slv_axi_clk, pcie_0_ldo, pcie_0_slv_q2a_axi_clk,
pcie_tbu_clk, pcie_phy_refgen_clk, pcie_ddrss_sf_tbu_clk
```

而 `step_clocks`（`alioth_pcie.c:318`）只投了五项：slv_axi、mstr_axi、cfg_ahb、aux、slv_q2a。
**缺的是 `pcie_0_ldo`、`pcie_phy_refgen_clk`、`pcie_0_ref_clk_src`、`pcie_tbu_clk`、
`pcie_ddrss_sf_tbu_clk`。** 其中 LDO 与 refgen 直接决定 core 与 PHY 的供电和参考时钟——
在它们没投票的情况下访问 IATU，最可能的后果就是总线不返回，与实测吻合。

下一步（不需要再猜）：把 `pcie_0_ldo` 与 `pcie_phy_refgen_clk` 的投票补上，其余不动，
只跑到 `step 9`（不碰 DBI）确认这两项不挂；确认后再开 `pcie=10`。这样断电次数从「每次未知」
变成「每一步可预期」。

## 追加：`elbi` 与 `dbi` 的映射重叠不解释挂死

窗口表（`alioth_pcie.c:88-98`）里 `elbi`（`0x60000f20` + `0xa8`）整个落在 `dbi`
（`0x60000000` + `0x1000`）之内，而 `map_all()` 是逐块各调一次 `ml_io_map`，不去重也不检查包含。
这看起来像「同一段物理地址被映射两次、可能拿到坏 VA」。

**但 `step_look`（第 1 步）就成功读了 `get(3, "elbi_sys_ctrl", 0x04)`**——同一个物理 4 KiB 页，
而且 `pcie=1` 到 `pcie=9` 全部通过。所以：

- 那段物理地址的映射与总线应答是好的；
- 重叠本身不会让读挂死，否则第 1 步就该挂；
- 挂死是 **`iatu`（`0x60001000`）这一页特有**的，而它与 `elbi` 不在同一页。

这把方向从「映射错」移到「IATU 这一侧的时钟或复位」。下一步仍是纯代码：读
`pcie_0_cfg_ahb_clk` / `pcie_0_slv_axi_clk` 的 branch 使能与 halt 状态（`step_clocks`
已读过一部分，属安全区），确认 IATU 所在的 AXI slave 分支有没有真的跑起来。

## 2026-10-30 夜：wifi1 轮，`pcie_0_ldo` 确实是关着的；DBI base 两种布局都是 0

`pcie=9 pcie_skip=32`，tag `wifi1`，kernel `dd657510…`，payload `fa1adb3e…`。
持久日志 `build/sheng-logs/wifi1-000000005fa365d6`（`header.txt` 的 `tag=wifi1` 把这次启动钉在这一版）。
桌面正常：`WALLPAPER stage=5 status=0`。

### 一、`pcie_0_ldo` 从来没被投过，投票后生效

| 读数 | 地址 | 值 |
| --- | --- | --- |
| step 1（本文件未写任何东西） | GCC `0x8c004` | **`0x80000000`** |
| step 3 投票后 | 同上 | **`0x1`** |

`0x80000000` 是 bit31 置位、bit0 清零：这个 branch **是关的**。
`gcc-kona.c:2521` 的 `gcc_pcie_wifi_clkref_en` 只有 `BIT(0)`，所以投票前它确实没跑。
这是本轮唯一一个新写入，并且它生效了。

**这条同时说明：`porting.toml` 里「LDO 决定 core 与 PHY 是否有电有钟」的判断，之前
从未被验证过**——现在可以验证了：投了之后 step 8/9 的 PARF 读数与之前完全一致
（`sys_ctrl 0x2000361c`、`phy_ctrl 0x900`、`ltssm 0`），所以 CLKREF 不影响 root complex
自己的寄存器访问。它影响的是端点侧的参考时钟，即链路训练。

### 二、`pcie_phy_refgen_clk` 不是缺失项

`GCC 0x6f02c` 在 step 1、6、9 全部读作 `0x1`，任何写入之前就是开的。
brief 与 `porting.toml` 把它列为「缺两个」之一，与读数不符。

### 三、DBI base：0x168 与 0x350 都是 0，布局问题不解释 step 10

| 偏移 | 出处 | step 8 | step 9 之后 |
| --- | --- | --- | --- |
| `0x168` | mainline v2.3.2 | `0` | （step 9 写 0） |
| `0x350` | vendor `pci-msm.c:43` | `0` | `0` |

v3 布局的旁证仍在（`0x358 = 0x1000000`、`0x16c = 0`），但**两个候选偏移上的 DBI base 都是 0**，
所以「清零写错了寄存器」这条解释不了 step 10 的挂死。

### 四、`parf_ltssm` 仍是 0，init 没有启动训练

step 8 读 `0`，step 9 之后仍读 `0`。本文件从不写 `PARF_LTSSM` bit 8
（vendor 在 `pci-msm.c:4409` 写它才开始训练），所以「训练从未被启动」是事实而非推断。

### 五、`step_look` 的 PARF/PHY/ELBI 读取是死代码——推翻证据文档上一条结论

`step_look` 在 `alioth_pcie.c:272` 早退：`gdscr = 0x22f001`（bit31 清零）、
`misc = 0x4100040`（低 6 位 = 0，不是 `0x3f`），两个条件都满足早退。
该早退自 `71331fc`（第一版）就在。

所以本文档 §「`elbi` 与 `dbi` 的映射重叠不解释挂死」里那句
「**`step_look`（第 1 步）就成功读了 `elbi_sys_ctrl`**」是**错的**：
全部 `console-*.log` / `serial-*.log` 里 `elbi` 的值行数为 **0**。
`elbi`（`0x60000f20`）那一页**从未被读过**，不能拿它证明「同一页可读」。

### 六、下一步

1. 补 `PERST#`（TLMM 79）与 `WLAN_EN`（TLMM 20）的驱动 —— 两个从未做过的写入，各一轮。
   `alioth_pcie.c` 全文没有 GPIO 写入，文件头注释 `:32` 说 step 4 会释放 gpio 79，与代码不符。
2. 写 `PARF_LTSSM` bit 8 打开训练，然后才有 `parf_ltssm` 可读。
3. step 10 的挂死仍无解释；现在已排除「时钟投票」与「DBI base 清零错位」两条。
   剩下的候选是 IATU 页自身的访问许可，或 conf 窗口映射（我们 `0x1000`、vendor `0x100000`）。

## 2026-10-31 00:20：wifi2 轮，两块端点 pad 第一次被驱动

tag `wifi2`，`pcie=10 pcie_skip=32`，kernel `d9faf2fa…`。**这一轮被污染，见文末。**

### 一、两个 pad 在 bootloader 之后的状态（事实）

```
PCIE 10: tlmm gpio20 cfg (wlan-en) = 0x1      io = 0x0
PCIE 10: tlmm gpio79 cfg (perst)   = 0x1      io = 0x0
```

`io = 0` 表示 out 位为 0 且 in 位读回 0：**WLAN_EN 低（未使能）、PERST# 低（复位被按住）**。
这就是「端点一直没上电、一直在复位」的直接证据，此前只是推断。

### 二、step 10 写入后回读全部正确（事实）

```
gpio20 cfg = 0x3c3  (pull-up | func gpio | drive16 | OE)   io = 0x3  → WLAN_EN HIGH
gpio79 cfg = 0x201  (pull-down | func gpio | drive2 | OE)   io = 0x3  → PERST# RELEASED
```

两块 pad 都接受写入，且 `io = 0x3` 说明 out 位已置、in 位也读回 1（pad 真的被拉高）。

### 三、`parf_ltssm` 仍是 0（事实）

```
PCIE 10: parf parf_ltssm after the endpoint enable = 0
```

【推断】这是预期结果，不是失败：本文件从不写 `PARF_LTSSM` bit 8，而 vendor 正是写它才开始训练
（`pci-msm.c:4409`）。GPIO 让端点有电、出复位，训练的开关还没打开。
下一单变量应当是 `PARF_LTSSM` bit 8。

### 四、这一轮被污染（事实，必须记）

镜像里多了两个不属于本轮的参数，来自另一个会话 00:16 的提交 `6e32f37`：

```
alioth-bt=1
alioth-bl=1
```

wifi1 的镜像没有它们（逐字节核对过）。后果：本轮 `GPUROW` 与 `WALLPAPER` 行数为 0
（wifi1 分别是 323 与 19），`BLPHASE` 心跳停在 t=3s（wifi1 到 t=173s）。
`BLPHASE` 由 `boot_log_tick` 打印，而 autoreset 就住在同一个函数里
（`BootLog.c:1278-1305`），所以它停 = autoreset 从未武装 = 板子不会自己回 fastboot。
内核仍活着（串口持续有 `TOUCHREG polls=`），需要断电重启。

**不能据此归因**：两个变量同时变了。GPIO 那三行读数在显示路径出问题之前就已完成，
所以 §1–§3 的数据仍然有效；但「为什么 `BLPHASE` 停了」这一条，本轮无法回答。

### 五、下一轮（wifi3）

1. 构建前确认 `board.toml` 不含 `alioth-bt` / `alioth-bl`，否则又是两变量。
2. 单变量 = `PARF_LTSSM` bit 8，然后读 `parf_ltssm`。

## 2026-10-31 03:21：wifi3 轮，LTSSM 使能写不进寄存器，但镜像能自回

镜像 `boot-wifi3.img` sha256 `de0f7723…`，内核 `f80d9849…`，`pcie=11 pcie_skip=32`，
镜像里没有 `alioth-bt=`/`alioth-bl=`。串口 233 行 `PCIE`（`tmp/console-wifi3r3.log`）。

### 一、`parf_ltssm` 仍是 0，而写入本身没有生效（事实）

```
PCIE 11: about to write parf parf_ltssm (LTSSM_EN, bit 8 in) at 0xfffffff87bc231b0: 0x100
PCIE 11: parf parf_ltssm after LTSSM_EN (0xfffffff87bc231b0) = 0
```

写 `0x100`，回读 `0x0`：**bit 8 没粘住**。同一轮 step 9 对其它 PARF 寄存器的写入是生效的
（`device_type 0 → 0x4`、`slv_addr_space_size → 0x4000000`），所以不是 PARF 只读，
而是 `0x1b0` 这个寄存器不接受这一位。

【推断】最可能是这一位**写后自清**：vendor 写它之后只轮询 ELBI 的 `XMLH_LINK_UP`，
从不回读这一位（`pci-msm.c:4409-4424`），所以 vendor 代码里看不到它粘住的样子。

**判据要改**：不能再用 `parf_ltssm` 由 0 变非 0。应改用 vendor 的那个信号 ——
ELBI `0x60000f28` 的 `XMLH_LINK_UP`(bit 10) 与 state(bits 12-17)（`pci-msm.c:4419`）。
但那在 ELBI/DBI 页，即 step 12 会挂的块。

### 二、两块 pad 仍正确（事实）

```
PCIE 10: WLAN_EN is HIGH (io 0x3)
PCIE 10: PERST# is RELEASED (io 0x3)
```

端点有电、出复位是确定的；训练没起来卡在 LTSSM 使能这一层。

### 三、镜像能自己回 fastboot（事实，两次）

`SELF-RETURNED after 186s`（03:19）、`SELF-RETURNED after 195s`（03:24）。
所以 wifi2 那次不自回**不是** `alioth-bt`/`alioth-bl` 造成的。

### 四、串口抓取的两个真 bug（已修，`tmp/round-wifi3r2.sh`）

1. `round.sh` 把 `fastboot boot` 的非零返回当失败。03:02 那轮实际 boot 了
   （持久日志 `tag=wifi3`、`procs=202`、reset 路径执行完），脚本却 4 秒就 kill 抓取退出。
   正确判据是「板子是否离开 bootloader」。
2. `/dev/tty.usbmodem14101` **只在内核运行时存在**。`console-capture.py` 另有守卫：
   端口 mtime 须 ≥ 它启动时刻 −1s，否则判 stale 一直等。所以正确顺序是
   `fastboot boot` → 板子离开 bootloader → **立刻**起抓取，让守卫等新枚举。
   先等端口出现再起抓取会晚约 1 秒，被守卫判 stale（03:16 实测）。

## 2026-10-31 03:45：ELBI 读挂死，且 ELBI 与 DBI 同页——我原来的页假设是错的

镜像 `boot-wifi4.img` sha256 `3473b301…`，内核 `aa87f2c1…`（与 wifi3 的 `f80d9849…` 不同，
证明 step 12 真的编进去了）。`pcie=12 pcie_skip=32`，镜像无 bt/bl。

### 一、读 ELBI 的第一次访问就挂死（事实）

```
PCIE 12: read the link control block's status word, ...
PCIE 12: about to read elbi elbi_sys_ctrl at 0xfffffff734888f24
                                    ^ 最后一行，之后没有回读
```

物理地址 `0x60000f24`（elbi 块 `0x60000f20` + 偏移 `0x04`，`PCIE20_ELBI_SYS_CTRL`，
`pci-msm.c:90`）。`risky()` 已把这一行刷到介质，所以「停在哪」是确定的。
板子 33 s 后自己回 fastboot（`SELF-RETURNED after 33s`）——**不是 180 s 的定时路径**，
说明这次是复位而不是超时。

### 二、我上一轮的页判断是错的（必须更正）

我在 step 12 的注释里写过：

> ELBI (`0x60000f20`) 与 DBI (`0x60000000`) 同一页，而 iATU (`0x60001000`) 是下一页，
> 所以读 ELBI 不碰 step 13 会挂的那一页。

**页对齐算得对，推论错。** 实测：`0x60000f24` 这一读就挂。而它与 DBI 同页 ——
所以「DBI 那一页」本身就是不可访问的，不是 iATU 那一页特别。

**这反而缩小了范围**：既然 ELBI（同页、偏移 +0x4）都读不了，那么问题在
**这一整页的访问许可**，而不是某个寄存器或某个偏移。

### 三、这否掉了「用 ELBI 做判据」

`XMLH_LINK_UP` 在 `0x60000f28`，与 `0x60000f24` 同一页，**同样读不了**。
所以「用 ELBI 判链路状态」这条路在本板上不可行，除非先解决这一页为什么不可访问。

### 四、下一步要问的问题（不是试）

`elbi` 是 `blocks[]` 里独立的第 3 项，`ml_io_map(0x60000f20, 0xa8)`；
`dbi` 是第 6 项，`ml_io_map(0x60000000, 0x1000)`。**两个映射重叠**（elbi 整段落在 dbi 的
4 KiB 内）。step 1 从来没读过 elbi（早退），所以这个重叠从未被验证过。

候选解释，按代价排序：
1. **重叠映射**：同一物理页被 `ml_io_map` 两次，第二份 VA 可能属性不同或未生效。
   验证方法：只保留 `dbi` 那一份映射、去掉 `elbi` 项，再从 dbi 的 VA 读 `+0xf24`。
   这是**零新硬件风险**的改动（只改映射表）。
2. `conf` 窗口我们映射 `0x1000`、vendor 是 `0x100000`（`kona-pcie.dtsi:13`）——
   但那是 conf 不是 elbi，暂不相关。
3. 这一页真的需要某个前置（例如 `parf_dbi_base` 必须指向它）。

### 五、换 VA 不是变量（离线结论，2026-10-31 03:52）

曾考虑三个「换个映射试试」的实验：打印 elbi/dbi 两个 VA、读 dbi VA +0x0、去掉 elbi 映射后从 dbi 读
+0xf24。读 `osfmk/arm/io_map.c:106-140` 后三个都不成立：

`io_map` 每次调用都 `kmem_alloc` 一段新 VA 再 `pmap_map`，**从不按页去重**（`grep dedup|reuse` = 0）。
而两个映射的 `start_offset` 让它们落在**同一个物理页**：

| block | 传入 | 实际 pmap_map 范围 |
| --- | --- | --- |
| elbi | `0x60000f20` + `0xa8` | `0x60000000 .. 0x60001000` |
| dbi | `0x60000000` + `0x1000` | `0x60000000 .. 0x60001000` |

所以「打印两个 VA」会得到不同 VA 但不说明任何问题；「读 dbi VA +0x0」物理上就是 `0x60000000`，
与 elbi `+0xf24` 同一个 AXI 窗口。**VA 不是变量，物理地址才是。**

真正的分区：

| 物理 | 归属 | 实测 |
| --- | --- | --- |
| `0x01c00000` parf | 控制器自己的寄存器窗口 | 读得了（steps 8/9/11） |
| `0x01c06000` phy | 同上 | 读得了（steps 4/5） |
| `0x60000000..0x60002000` | DWC 的 dm_core/elbi/iatu 窗口 | **首次访问即挂** |
| `0x60100000` conf | 配置空间 | 从未读过 |

vendor DT 证实这三条是**一段连续 AXI 窗口**，只为 Linux 资源模型才拆分：
`dm_core 0x60000000+0xf1d`、`elbi 0x60000f20+0xa8`（紧接 0xf1d 之后）、`iatu 0x60001000+0x1000`。

两次独立测量都在这一段的首次访问挂死：本轮 elbi `+0xf24`，旧 `pcie=10` 轮 iatu `+0x1000`
（`iatu_viewport` 在所有 console 里没有回读行）。`elbi` 更是一次都没被读过。

**下一步单变量**：把 `parf_dbi_base` 的零写同时写到 `0x168` 与 `0x350`，读回比对，再试一次 ELBI 读。
这一段窗口的基址由 `parf_dbi_base` 决定，而我们只写过 v2 的 `0x168`，v3 的 `0x350` 从未被写。
写的是 PARF（已证明可写），不是那段死窗口。

## 2026-10-31 04:02：两个 DBI base 偏移都写进去了，ELBI 仍然挂

镜像 `boot-wifi5.img` sha256 `9c5fc01c…`，内核 `13c77bee…`（与 wifi4 的 `aa87f2c1…` 不同），
`pcie=12 pcie_skip=32`，无 bt/bl。236 行 PCIE（`tmp/console-wifi5r1.log`）。

### 一、判据落在三态里的第二态（事实）

```
PCIE 9: parf parf_dbi_base 0x168 read back = 0     <- 写进去了
PCIE 9: parf parf_dbi_base 0x350 read back = 0     <- 写进去了
PCIE 12: about to read elbi elbi_sys_ctrl at ...   <- 之后没有回读
```

两个偏移都接受写入并回读为 0，**ELBI 的首次访问仍然挂死**。板子 36 s 后回来（复位，非 180 s 定时）。

**结论：`parf_dbi_base` 不是那一段窗口不可访问的原因。** 这一段的上游另有问题。

### 二、一条我自己差点报错的读数（更正）

我看到 `slv_axi_halt` / `mstr_axi_halt` 读作 `0x220`，一度判定「bit 5 置位 = halted」。
**错了。** `clk-branch.h:34` 的 `BRANCH_HALT` 是「1 = halt」，但测试用的是 `halt_bit` 字段
（`clk-branch.c:40-45`：`val &= BIT(br->halt_bit)`），而 `gcc_pcie_0_slv_axi_clk`
（`gcc-kona.c:2288-2295`）**没有设 `halt_bit`，默认 0**。

`0x220` 的 bit 0 是 **0** → 按 vendor 定义这条分支**没有 halt**，时钟在跑。
`0x220` 里的 bit 5 与 bit 9 是别的字段，与 halt 判定无关。

**所以时钟不是原因**，我差点据此写出一条错误结论。

### 三、下一步的候选（都不是时钟）

1. **`parf_slv_addr_space_size`（`0x358`）**：已写 `0x4000000` 并回读正确。但这是「slave 地址空间大小」，
   决定这段窗口对外可见的地址范围。若它必须**包含** `0x60000000` 这一段的基址，而基址现在是 0，
   那窗口的地址映射本身就不成立。
2. **`parf_axi_mstr_wr_addr_halt_v2`（`0x1a8`）**：已写 bit31，回读 `0x80000014`。
   vendor 用它「halt the master's write address」，可能必须在某一步**释放**而不是置位。
3. 这一段可能依赖 `conf` 窗口（`0x60100000`）先映射——从未读过，也从未验证。
4. **最根本的**：我们从未验证过这一段在**任何**内核状态下可读。`elbi` 一次都没读过，
   `iatu` 只在会挂的那一轮被碰过。所以「它可读」这个前提本身从未成立过。

## 2026-10-31 04:08：「我们自己 halt 了 AXI master」这条假设不成立

有一条提议认为根因是 `parf_axi_mstr_wr_addr_halt_v2`（`0x1a8`）的 bit 31 —— 我们置了它、从没清过，
所以 master 的写地址被 halt，`0x60000000..0x60002000` 那段访问不返回。**三条独立证据否掉它。**

### 一、写入前读到的就已经是 `0x80000014`（事实）

```
console-wifi5r1.log:259  parf_axi_mstr_wr_addr_halt_v2 (...) = 0x80000014
console-wifi5r1.log:260  about to write ... (bit 31 in) ... : 0x80000014
```

`step_parf_init` 是「读 → `value | (1u<<31)` → 写」。读到的 `0x80000014` 已经含 bit 31，
所以那一写**写回的是同一个值，是空操作**。**不是我们置的位。**

### 二、mainline 也置这一位，而且没人清（事实）

- `pcie-qcom.c:656-658`（2.3.2）与 `:943-945`（2.4.0）：`val |= BIT(31); writel(val, ...)`
- `pcie-qcom.c:513-521`（2.1.0）：同样，只是包在 `CONFIG_PCI_MSI` 里
- 全部 `grep WR_ADDR_HALT | &=|clear|~` → **空**，vendor 与 mainline 都不清

如果 bit 31 是「永久 halt」，这些 SoC 上 Linux 的 PCIe 全都不能用。**它实际是「启用写地址 halt 机制」的使能位**，
不是「现在就 halt」——寄存器名 `AXI_MSTR_WR_ADDR_HALT` 是「halt 地址」寄存器，bit31 使能，低位是 halt 尺寸/地址。

### 三、vendor 写的就是我们读到的值（事实）

`pci-msm.c:4561-4564`：`val = dev->wr_halt_size ? wr_halt_size : readl(0x1a8); write(BIT(31)|val)`。
本板 DT 没有 `wr-halt-size`，所以 vendor 也是「读回 `0x14`，写 `0x80000014`」。**我们与 vendor 逐位一致。**

### 四、结论

`0x1a8` bit 31 **不是**那一段窗口不可读的原因。回到原来的候选：这一段从未被证明可读
（`elbi` 一次没读过，`iatu` 只在会挂的那轮被碰过），以及 `parf_slv_addr_space_size` /
`conf` 窗口映射这两条尚未验证的前置。

**教训**：一个「我们置了某位从没清」的机制听起来很像根因，但**读一遍写入前的值**就能判掉一半。
本轮这一步只花了一次 grep。

## 2026-10-31 04:12：与 vendor 的差异表——两行需要更正，一行是真差异

有一条提议说我们「从没写过 `SYS_CTRL`、也没写过 `MHI_CLOCK_RESET_CTRL` bit 4」。**这两行不对。**

### 一、这两步我们都做了（事实）

```
alioth_pcie.c:652-653  value = get(1, "parf_sys_ctrl", 0x00);
                       put(1, "parf_sys_ctrl (bit 29 out, core clock gating off)", 0x00, ...)
alioth_pcie.c:670-671  value = get(1, "parf_mhi_clock_reset_ctrl", 0x174);
                       put(1, "parf_mhi_clock_reset_ctrl (bit 4 in)", 0x174, value | (1u << 4))
```

`console-wifi5r1.log:245,246,256,257` 是本轮的实测行。

### 二、但这两处**从未回读验证**（事实）

全文只有 `about to write`，没有 `after`。所以「写了」是确定的，「生效了」不确定。

### 三、真差异在 `SYS_CTRL` 的写法上

| | 值 | 说明 |
| --- | --- | --- |
| bootloader 留下 | `0x2000361c` | bits 2,3,4,9,10,12,13,29 |
| **vendor（pci-msm.c）** | 读改写，只清 bit29 | → `0x361c` |
| **mainline 2.3.3（pcie-qcom.c:1096-1099）** | 硬编码 `0x3656` | bits 1,2,4,6,9,10,12,13 |
| **我们** | 硬编码 `0x365e` | mainline 的值 **+ bit3** |

我们比 mainline 多保留了 **bit 3**（bootloader 留下的），而 vendor 那条路径只清 bit 29、保留其它全部。
`bit3` 在 `pcie-qcom.c:32-40` 里**没有定义**，所以它是什么、该不该保留，本树无法回答。

### 四、`MHI_CLOCK_RESET_CTRL` 我们与 vendor 一致

`0x3 → 0x13`（置 bit 4），`pci-msm.c:4560` 与 `pcie-qcom.c:653-655` 都是这个写法。**这一步没有差异。**

### 五、结论

与 vendor 的差异**不在「有没有写这两个寄存器」**（都写了），而在：

1. `SYS_CTRL` 我们写的是 mainline 的硬编码值 + bit3，vendor 是「只清 bit29」的读改写；
2. 这两处**都没有回读验证**，所以「写进去没有」未知。

**下一步单变量**：给这两处各加一次回读，确认值真的落地。这是零风险（只加读），而且能把「写了但没生效」
与「生效了仍不通」分开 —— 后者才是那一段窗口的问题。

## 2026-10-31 04:18：DT 核对——地址是对的，不是从别的板抄的

在动板子之前先排掉「地址本身错了」这个根本假设。

### 一、真机 DTB 里的 pcie0 节点（事实）

从**启动用的那份** `uefi-alioth/Resources/DTBs/alioth.dtb`（933,795 字节）反编译：

```
pcie0: qcom,pcie@1c00000 {
    compatible = "qcom,pci-msm";
    reg = <0x1c00000 0x3000  0x1c06000 0x1000  0x60000000 0xf1d
           0x60000f20 0xa8  0x60001000 0x1000  0x60100000 0x100000>;
    reg-names = "parf", "phy", "dm_core", "elbi", "iatu", "conf";
    perst-gpio = <0x65 0x4f 0x00>;     /* TLMM 79 */
    wake-gpio  = <0x65 0x51 0x00>;     /* TLMM 81 */
```

`0x60000000` 这一段**确实在这块板的 DWC 窗口**里，且 `perst-gpio` 的 0x4f(79) 与代码一致。

### 二、为什么这是「这块板」而不是「参考板」（事实）

`reference/k40-kernel/.../qcom/Makefile:19`：

```
alioth-sm8250-overlay.dtbo-base := kona.dtb kona-v2.dtb kona-v2.1.dtb
```

**alioth 的 DT 就是 `kona.dtb` 加 overlay**，而 `kona.dtb` 由 `kona.dtsi` 编译，
`kona.dtsi:5167` include 了 `kona-pcie.dtsi`。所以源码与真机 DTB 两条独立路径都指向同一组地址。

`alioth-sm8250.dtsi` / `xiaomi-sm8250-common.dtsi` **没有覆盖 pcie0**（`grep pcie` 空），
即这块板用的是 `kona-pcie.dtsi` 的原值，没有改过。

### 三、结论

**地址假设成立，可以继续。** 下一步是把 ELBI 的那一次读挪到 step 2（控制器上电之后、任何时钟投票之前），
二分出「这一段是从来就不可访问，还是被我们某一步关掉的」。

**注意我们自己的 DTS 里没有 pcie 节点**（`NEW/UniXNU-alioth/dts/alioth.dts` 与
`NEW/UniXNU-sm8250-common/dts/*.dtsi` 的 `grep pcie` 全空）——所有地址只存在于
`package/alioth_pcie.c` 的常量里。所以「DT 对不对」这个问题，对本轮的代码路径**不构成输入**；
上面核对的是「这些常量抄得对不对」，答案是对。

## 2026-10-31 04:17：window probe 在控制器上电前就挂——这一段从 CPU 侧不可访问

镜像 `boot-wifi6.img` sha256 `4f527114…`，内核 `6ad614ad…`（与 wifi5 的 `13c77bee…` 不同），
`pcie=12 pcie_skip=32`，无 bt/bl。30 行 PCIE（`tmp/console-wifi6r1.log`）。

### 一、`step_look` 末尾那行 ELBI 读从来没执行过（事实，本轮之前）

`alioth_pcie.c` 的 `step_look` 末尾有 `get(3, "elbi_sys_ctrl", 0x04)`，但它被早退挡住：
`gdscr = 0x22f001`（PWR_ON 清）、`misc = 0x4100040`（低 6 位为 0，不是 `0x3f`）→ 每轮都早退。
**从第一版提交到现在，这一行一次都没跑过。** 所以「这一段可读吗」这个问题从未被测过。

### 二、把那一读挪到早退之前，它挂了（事实）

本轮把那行读到 `printf` 与早退**之前**，用控制器仍在塌陷、任何时钟都没投票的状态试：

```
PCIE 1: gcc pcie_0_bcr (core reset) = 0
PCIE 1: gcc pcie_0_phy_bcr (phy reset) = 0
   <- 日志到这里为止，没有 printf 那行，也没有 window probe 的 "about to read"
```

最后一行是 `phy_bcr` 的**成功读回**。之后 `printf("the controller is %s...")` **没有出现**
（`grep -c "the controller is"` = 0），而 printf 不碰硬件。所以死在 **`get(3, elbi_sys_ctrl, 0x04)`
这一读**上——它自己的 `line("about to read ...")` 也没来得及出现，因为 `risky()` 之后的访问不返回。

板子 **39 s** 后回来（复位，非 180 s 定时）。

### 三、判据落在二值的第一态

| step 1 读 ELBI | 含义 |
| --- | --- |
| **就挂（本轮）** | 这一段在控制器**上电之前**就不可访问，与本文件做的任何初始化无关 |
| 读得出 | 可读，是后面某步关掉的 |

**所以：`0x60000000..0x60002000` 在控制器塌陷状态下就已经不可访问。**
「某一步把它关掉了」这个解释被排除——那时还什么都没做。

### 四、这否掉了什么，剩下什么

否掉：
- 「时钟投票漏了某一档」——那时一个投票都还没做
- 「SYS_CTRL / MHI_CLOCK_RESET_CTRL 写错」——那时一个都还没写
- 「`parf_dbi_base` 没设」——那时还没设，但 wifi5 证明设了也一样

剩下（按代价）：
1. **这段地址在 XNU 的 IO 映射里是否成立**。`ml_io_map` 走的是 `io_map` → `pmap_map`，
   它对 `0x60000000` 这段物理地址建立的映射，可能因 IOMMU/SMMU 或 physmap 边界而无效。
   **注意 parf (`0x01c00000`) 可读**，而两者走同一个 `ml_io_map`，所以不是「整个 io_map 坏了」，
   是**这段物理地址**特别。
2. 这段是否需要**上游供电/复位**（例如 `dm_core` 所在的某个 GDSC 或 NoC 端口）在访问前必须打开。
   `gdscr` 是 PCIe 控制器自己的电源开关，但这段窗口属于 DWC，可能有独立的上游。
3. 地址本身是否有一个偏移或别名（例如实际基址不是 `0x60000000`，而是经过某个 ATU 映射后的）。

**下一步建议**：先在**不碰这段窗口**的前提下验证第 1 条——用一个已知可读的物理地址
（例如 parf 的 `0x01c00000`）与这段一起做 `ml_io_map`，比较两者返回的 VA 与 pmap 结果。
但那需要读 `pmap` 内部状态，本文件没有这个能力。

更实际的做法：**查 vendor 在访问这段之前是否做过某个我们不做的映射**（`dma-coherent`、
`iommu-map`、或 `iommu_dma` 配置）。本板 DT 里 pcie0 有 `dma-coherent` 与
`qcom,smmu-sid-base = 0x1c00`，而**本树没有 SMMU 的 SID 配置**。

## 2026-10-31 04:23：控制器有电之后，window probe 仍然挂——电不是变量

镜像 `boot-wifi7.img` sha256 `70bb2fe4…`，内核 `cbb7e07b…`（与 wifi6 的 `6ad614ad…` 不同），
`pcie=12 pcie_skip=32`，无 bt/bl。38 行 PCIE（`tmp/console-wifi7r1.log`）。

### 一、把 probe 从 step 1 挪到 step 2（消掉一个变量）

wifi6 把 ELBI 读放在 step 1 的早退之前，那时控制器**仍在塌陷**且**一个时钟都没投票**——
两个条件同时成立，挂死说明「这一状态不可访问」，但说不清是哪个条件造成的。

本轮把同一读挪到 `step_gdsc` 末尾：控制器已上电，step 3 还没投票。**只剩一个变量：电。**

### 二、结果：仍然挂（事实）

```
PCIE 2: gcc pcie_0_gdscr after (...) = 0xf822f000
PCIE 2: THE CONTROLLER HAS POWER (gdscr 0xf822f000, 1 ms)
   <- 日志到此为止
```

`0xf822f000` 的 bit31 置位（PWR_ON），控制器确实有电。之后 probe 的
`about to read` 行**没有出现**（`grep -c` = 0），板子 39 s 后复位回来。

### 三、两个变量都排掉了

| 状态 | probe 结果 |
| --- | --- |
| 塌陷（gdscr 0x22f001，无投票） | **挂**（wifi6） |
| 有电（gdscr 0xf822f000，无投票） | **挂**（本轮） |

**所以「控制器没上电」和「时钟没投票」都不是这一段不可访问的原因。** 剩下的是地址/映射本身。

### 四、顺带确认：六个投票位一个都没置（事实）

step 1 现在会打印：

```
PCIE 1: the clock votes are 0, all six are voted: NO -- none of slv_axi, mstr_axi,
        cfg_ahb, aux, slv_q2a is voted yet
```

`misc = 0x4100040`，低 6 位全 0。这解释了 `step_look` 为何每轮早退——**但它不是窗口不可访问的原因**
（本轮已证明：有电、仍挂；而 wifi5 已证明：step 3 投了 5 个位之后，step 12 仍挂）。

### 五、下一步

只剩两条，都不是「再试一个寄存器」：

1. **地址/映射本身**。`parf`（`0x01c00000`）走同一个 `ml_io_map` 却可读，所以映射机制没问题，
   是这段物理地址特别。要查的是 `io_map` 为 `0x60000000` 建立的映射是否真的指向了总线上那个从端口
   ——例如 SMMU 的 SID 配置（本板 DT 有 `qcom,smmu-sid-base = 0x1c00` 与 `iommu-map`，
   而**本树没有 SID 配置**）。
2. **上游**：这段窗口属于 DWC，可能有一个 `gdscr` 之外的电源/复位域（例如 NoC 端口或 CX 域）。

**建议先查第 1 条，零成本、不用板子。**

## 2026-10-31 04:30：vendor 在第一次读 ELBI 之前，先对 dm_core 写了六次

离线核对（不用板子）。这条把「这一段不可访问」的下一步指向了一个具体方向。

### 一、vendor 的第一次 ELBI 读在 `msm_pcie_link_train` 里（事实）

`pci-msm.c` 里对 elbi 的第一次访问是 **4419 行**，在 `msm_pcie_link_train`（从 4374 起）内部：

```
4416  do {
4417      usleep_range(LINK_UP_TIMEOUT_US_MIN, LINK_UP_TIMEOUT_US_MAX);
4419      val = readl_relaxed(dev->elbi + PCIE20_ELBI_SYS_STTS);
```

**它前面是六次 `dm_core` 写**（同函数 4379-4412）：

```
4379  msm_pcie_write_reg_field(dm_core, PCIE_GEN3_GEN2_CTRL, 0x1f00, 1);
4382  msm_pcie_write_mask(dm_core, PCIE_GEN3_EQ_CONTROL, 0x20);
4386  msm_pcie_write_mask(dm_core + PCIE_GEN3_RELATED, BIT(0), 0);
4390  msm_pcie_write_reg_field(dm_core, PCIE_GEN3_MISC_CONTROL, BIT(0), 1);
4393  msm_pcie_write_reg(dm_core, PCIE_GEN3_SPCIE_CAP, dev->core_preset);
4395  msm_pcie_write_reg_field(dm_core, PCIE_GEN3_MISC_CONTROL, BIT(0), 0);
4409  msm_pcie_write_mask(parf + PARF_LTSSM, 0, BIT(8));      <- 训练使能
4419  readl(elbi + ELBI_SYS_STTS)                             <- 第一次读 ELBI
```

### 二、这意味着窗口在那时是**通的**（事实）

vendor 能对 `dm_core`（`0x60000000`）连续写六次，然后才读 `elbi`（`0x60000f20`）。
**写能进 = 那一段在 vendor 的时序里是通的。** 所以「这段物理上不可访问」这个结论不成立——
它在某个状态下是可访问的，只是我们从未到达那个状态。

### 三、我们从未对 dm_core 做过任何读写（事实）

`grep` `alioth_pcie.c` 全文：`dm_core` / `dbi` 只出现在**注释**和 `blocks[]` 的映射表里，
**没有任何一次 `get()` 或 `put()` 碰过它**。`step 13`（DBI）是唯一会碰的，而它从未跑过。

### 四、所以下一步不是「再试一个寄存器」

vendor 的时序里，**六次 GEN3 preset 写**是 ELBI 可读之前发生的事。我们的 `pcie=12` 在 step 12
（读 ELBI）之前没有这些写。这与 wifi5/6/7 的结果完全吻合：
**每一个我们能到达的状态，都还没做过 vendor 做的那六次写。**

**下一步单变量**：在 step 12 之前补上这六次 `dm_core` 写（`PCIE_GEN3_*` 那组），再读 ELBI。
这是**第一次写** `0x60000000` 这段窗口，风险比之前几轮高——但它正是 vendor 在同一个位置做的事，
而且 vendor 的 `pcie_phy_init` 已在 step 4/5 完成。

如果那六次写**也挂**，那才是「这段在我们这里真的不可访问」，要回去查映射；
如果写得进、ELBI 仍读不出，那是 ELBI 那个偏移的事，不是整段。

## 2026-10-31 04:35：映射属性也是对的——`VM_WIMG_IO` 在非 DRAM 上给 Device-nGnRnE

离线核对（不用板子）。这条排掉「映射属性错，访问触发同步异常」这个解释。

### 一、我们的调用路径（事实）

`ml_io_map` → `io_map(phys, size, VM_WIMG_IO, VM_PROT_DEFAULT, false)`
（`machine_routines.c:1621-1626`）。

我们的驱动跑在 `native_legacy_display_late_probe` 里，`startup_phase >= STARTUP_SUB_KMEM`，
所以走 `io_map.c:124-141` 的 else 分支：`kmem_alloc(io_submap, ...)` + `pmap_map(..., flags)`。
**不是 `pmap_map_bd`。**

### 二、`pmap_map` 把 flags 一路传到底（事实）

`pmap_map` → `pmap_enter` → `pmap_enter_addr` → `wimg_to_pte(wimg, pa)`（`pmap.c:5351-5366`）：

```c
case VM_WIMG_IO:
    if (is_dram_addr(pa)) {
        pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_POSTED_COMBINED_REORDERED);
    } else {
        pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DISABLE);      // Device-nGnRnE
    }
    pte |= ARM_PTE_NX | ARM_PTE_PNX;
```

`0x60000000` 不是 DRAM（本板 DRAM 从 `0x80000000` 起，`alioth.dts:14-17`），
所以得到 **`CACHE_ATTRINDX_DISABLE` = Device-nGnRnE** —— 这正是访问 MMIO 寄存器该用的属性。

### 三、`pmap_enter` 不校验物理范围（事实）

`pmap_enter`（`pmap.c:5152`）只是 `pmap_enter_addr(pmap, v, (pmap_paddr_t)pn << PAGE_SHIFT, ...)`，
**没有 `gPhysBase` / `real_phys_size` 的范围检查**（那些检查在 `phystokv`，`arm_vm_init.c:383,408`，
是另一条路径）。所以这段地址能被建立映射，不会因为「不在 DRAM 内」被拒。

### 四、结论

**映射属性是对的，映射也确实建立了。** 所以「属性错导致同步异常」这条不成立。

这与 `parf` 可读完全一致：两者走同一条路径、同一个属性。

### 五、剩下唯一没查的一条

vendor 在第一次读 ELBI 之前**先对 dm_core 写了六次**（`pci-msm.c:4379-4395`，见上一条记录），
而我们**从未对 dm_core 做过任何读写**。这是唯一还站着的解释：
**这段窗口在 vendor 的时序里是通的，我们只是从未到达那个状态。**
