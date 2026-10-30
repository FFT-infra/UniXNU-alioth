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

## 2026-10-31 04:40：第一次 dm_core 访问就挂——窗口在我们这里不可访问

镜像 `boot-wifi9.img` sha256 `c223f758…`，内核 `8a2ee482…`，`pcie=12 pcie_skip=32`，无 bt/bl。
241 行 PCIE（`tmp/console-wifi9r1.log`）。

### 一、wifi8 白跑了一轮（我的疏忽，先记）

wifi8 想跑 step 12 的七次 dm_core 写，但 wifi7 的 **step 2 window probe 还留在代码里**，
它在 step 12 之前就挂死了。实测 `console-wifi8r1.log` 只有 39 行，最后一行是
`about to read elbi elbi_sys_ctrl (window probe, powered, no votes yet)`。
**step 12 一次都没执行。** 本轮已把那个 probe 删掉（它的问题 wifi6/wifi7 已答完）。

### 二、第一次 dm_core 访问就挂（事实）

```
PCIE 12: the seven dm_core writes ... the first access this file has ever made to 0x60000000
PCIE 12: about to read dbi dwc gen3_gen2_ctrl before at 0xfffffff45450380c
   <- 到此为止，没有值
```

`0xfffffff45450380c` 是 dbi 块 VA + `0x080c`（`DWC_GEN3_GEN2_CTRL`）。
`risky()` 已把这一行刷到介质，所以停在哪是确定的：**读不返回**。
板子 36 s 后复位回来。

### 三、这是三态里最重的一态

| 结果 | 含义 |
| --- | --- |
| **第一次 dm_core 访问就挂（本轮）** | 那段在**我们这里**不可访问，与 vendor 不同 |
| 写进得去、ELBI 读不出 | ELBI 那个偏移的事 |
| 写进得去、ELBI 读得出 | preset 是前提 |

**所以「vendor 写了六次所以我们也能」这个外推被否掉了。** vendor 的环境和我们的不同——
不是我们漏了一步，而是**我们在自己的环境里到不了那个状态**。

### 四、这一条同时否掉了「只是 ELBI 那个偏移」的解释

之前 wifi4 读的是 ELBI（`0x60000f24`），本轮读的是 dm_core（`0x600000380c`），**两个不同偏移、都挂**。
所以不是某个寄存器的问题，是**整段窗口**。

### 五、现在还剩什么

前面已排掉：SMMU（不经）、VA 记账（正确）、映射属性（Device-nGnRnE 正确）、
没上电（wifi7）、没投票（wifi5/7）、**「漏了 vendor 的写」（本轮）**。

剩下的是**这一段物理地址在本内核的 IO 映射里是否真的可达**——
但 `parf` 走同一路径可读，所以要么这段有独立的门控，要么地址本身不对（尽管 DT 与真机 DTB 都对）。

**一个此前未做的检查**：`conf`（`0x60100000`）也从未读过。它在同一 SoC 的不同窗口，
如果它也挂，那说明问题不在「`0x60000000` 这段」，而在更普遍的东西（例如所有非 parf/phy 的窗口）。

## 2026-10-31 04:48：conf 窗口也挂——问题不在 0x60000000 这一段

镜像 `boot-wifi10.img` sha256 `cb1f4e75…`，内核 `8a2ee482…`，`pcie=12 pcie_skip=32 pcie_probe=conf`，
无 bt/bl。241 行 PCIE（`tmp/console-wifi10r1.log`）。

### 一、结果（事实）

```
PCIE 12: probe the configuration window (0x60100000) before dm_core, ...
PCIE 12: about to read conf conf_vendor_device (window probe) at 0xfffffff799add000
   <- 到此为止，没有值
```

`0xfffffff799add000` 是 conf 块的 VA（`0x60100000`）。板子 36 s 后复位。

### 二、这是「更普遍」那一支

`pcie_probe=conf` 让 step 12 只做这一次读并返回，所以本轮**没有碰 dm_core**。判据是：

| conf | 含义 |
| --- | --- |
| **挂（本轮）** | 问题不在 `0x60000000` 这一段，在更普遍的东西 |
| 读得出 | 问题就是这一段独有 |

**所以「`0x60000000..0x60002000` 这段地址特别」这个结论也不成立。** conf（`0x60100000`）是
同一控制器上的**另一个窗口**，也挂。

### 三、现在三个窗口的状态

| 窗口 | 物理 | 实测 |
| --- | --- | --- |
| parf | `0x01c00000` | **读得了**（steps 8/9/11） |
| phy | `0x01c06000` | **读得了**（steps 4/5） |
| dm_core | `0x60000000` | **挂**（wifi9） |
| elbi | `0x60000f20` | **挂**（wifi4） |
| iatu | `0x60001000` | **挂**（旧 pcie=10 轮） |
| **conf** | **`0x60100000`** | **挂（本轮）** |

**分界不是「哪一段地址」，是「哪一类窗口」**：`0x01c00000` 那组（控制器自己的寄存器）可读，
`0x60000000` 与 `0x60100000` 这组（DWC 的地址空间）全挂。

### 四、这与「没上电/没投票/没写 dm_core」全部无关

本轮 `pcie=12` 在 step 12 就停，前面 steps 1-11 全跑完（含 step 2 上电、step 3 投票、step 9 写 PARF）。
所以：
- 控制器**已上电**（step 2）
- 时钟**已投**（step 3）
- PARF **已写**（step 9，回读正确）

**这些状态下 conf 仍挂。** 所以前面所有排除依然成立，而且现在指向的不是「某段地址」，
而是**DWC 地址空间整体的可达性**。

### 五、下一步

`parf` 与 `phy` 都在 `0x01c00000` 附近，属于控制器自己的寄存器块；
`dm_core`/`elbi`/`iatu`/`conf` 都在 `0x60000000` 以上，属于 DWC 的地址空间。

**要问的是：这一整片 DWC 地址空间，在本内核里是否需要某个前置才能访问？**
候选（都不是「再试一个寄存器」）：
1. 这片地址是否经过某个 AP 侧的 NoC/AXI 端口，而该端口需要单独的 power/clock vote
   （不是 PCIe 的 `gdscr`，是 SoC 级的）。
2. 这片地址是否需要 `parf_slv_addr_space_size`（`0x358`）之外的东西先设。
3. 这片地址的基址是否需要先由 PARF 的某个寄存器指过去（我们已试 `parf_dbi_base`，无效）。

**但还有一个更基本的可能**：这整片 `0x60000000+` 在 XNU 的 physmap/IO 视图里可能根本不可达
——`parf`/`phy` 可读可能只是因为它们在某个默认范围内。

## 2026-10-31 04:55：`pmap_enter` 不会对 `0x60000000` 静默失败

离线核对（不用板子）。这条排掉「映射没建出来」这个解释。

### 一、`pmap_enter` 的完整检查链（事实）

`pmap_enter`（`pmap.c:5142`）→ `pmap_enter_addr`（`:5115`）→ `pmap_enter_options_addr`（`:5922`）
→ `pmap_enter_options_internal`（`:5534`）。

唯一的物理地址检查是**对齐**（`pmap.c:5572-5575`）：

```c
if ((pa) & pt_attr_leaf_offmask(pt_attr)) {
    panic("pmap_enter_options() pmap %p pa 0x%llx", pmap, (uint64_t)pa);
}
```

`0x60000000` 是页对齐的，所以这条不触发。

**没有任何 `gPhysBase` / `real_phys_size` 范围拒绝。** 全文搜 `gPhysBase`，只有
`pmap_data.c:406,549` 与 `pmap.c:11463`，都不在 `pmap_enter` 这条路径上。

### 二、`is_dram_addr` 只决定属性，不拒绝（事实）

`vm_param.h:303`：
```c
#define is_dram_addr(addr)  (((uint64_t)(addr) - gDramBase) < gDramSize)
```
它只被 `wimg_to_pte`（`pmap.c:5361,5373,5381,5391`）用来选属性，**从不作为拒绝条件**。

`gDramBase`/`gDramSize` 来自 DT 的 `/chosen` 节点 `dram-base` / `dram-size`（`arm_init_sptm.c:892-893`）。

### 三、最后一层 `sptm_map_page` 不在本树

`pmap_enter_pte` 里对它的调用是 `__assert_only`（`pmap.c:1405,1497,4898`），
即**只在 assert 构建里检查返回值**；它的实现由固件提供，本树没有。

**所以「`pmap_enter` 静默失败」这条在本树内无法成立**：代码里没有任何会吞掉这段地址的检查。

### 四、结论

`0x60000000` 的映射**是建出来的**（属性 Device-nGnRnE，VA 在 `io_submap` 内）。
所以「VA 指向未映射页」这个解释排掉。

**这与 `parf` 可读、DWC 窗口全挂完全一致**：两者的映射都建成了，差别在**总线上有没有应答**。

### 五、条件表更新（第 7 条也排掉了）

| 解释 | 状态 |
| --- | --- |
| SMMU SID | 排除 |
| VA 记账错 | 排除 |
| 映射属性错 | 排除 |
| **映射未建立 / 静默失败** | **排除（本条）** |
| 没上电 | 排除（wifi7） |
| 没投票 | 排除（wifi5/7） |
| 漏了 vendor 的 dm_core 写 | 排除（wifi9） |
| **「只是某段地址」** | **排除（wifi10：conf 也挂）** |

剩下的是**这片 DWC 地址空间在总线上没有应答**——即 AP 侧到 DWC slave 端口之间有一个我们没打开的门。

## 2026-10-31 05:00：vendor 在读 dm_core 之前使能了两路 PMIC 稳压器，我们从未做过

离线核对（不用板子）。这条给出条件表上第 9 条，而且它有具体的机制，不是「也许某条件」。

### 一、地址常量与真机 DTB 逐项一致（事实，先排除「地址抄错」）

| 项 | `blocks[]` 常量 | 启动 DTB 的 `reg` |
| --- | --- | --- |
| parf | `0x01c00000` + `0x3000` | `0x1c00000 0x3000` |
| phy | `0x01c06000` + `0x1000` | `0x1c06000 0x1000` |
| dm_core | `0x60000000` + `0x1000` | `0x60000000 0xf1d` |
| elbi | `0x60000f20` + `0xa8` | `0x60000f20 0xa8` |
| iatu | `0x60001000` + `0x1000` | `0x60001000 0x1000` |
| conf | `0x60100000` + `0x1000` | `0x60100000 0x100000` |

**逐项对上**（`dm_core` 我们映射 4 KiB、vendor 声明 `0xf1d`，不影响基址）。
所以「常量抄错」这条排除。

### 二、vendor 在第一次碰 dm_core 之前做了什么（事实）

`msm_pcie_enable`（`pci-msm.c:4477`）里，顺序是：

```
4477  gpio_set_value(PERST, assert)
4483  msm_pcie_vreg_init(dev)          <-- PMIC 稳压器
4489  msm_pcie_clk_init(dev)           <-- GCC 时钟
4499  write_reg(parf, DEVICE_TYPE, 4)
4508  write_mask(parf, PHY_CTRL, BIT(0), 0)
4511  write_reg(parf, DBI_BASE_ADDR, 0)
4513  write_reg(parf, SYS_CTRL, 0x365E)
4516  write_mask(parf, MHI_CLOCK_RESET_CTRL, 0, BIT(4))
...
4558  write_reg(parf, SLV_ADDR_SPACE_SIZE, ...)
4561  write_reg(parf, AXI_MSTR_WR_ADDR_HALT, BIT(31)|val)
4574  pcie_phy_init(dev)
...
4611  readl(dev->conf)                 <-- 第一次碰 0x60100000
```

**除了 parf，vendor 还做了两件我们没做的事**：
1. `msm_pcie_vreg_init`（4483）—— PMIC 稳压器
2. `msm_pcie_clk_init`（4489）—— 我们做了时钟，见下

### 三、`msm_pcie_vreg_init` 的两路必开轨（事实）

`pci-msm.c:930-935`：
```c
{NULL, "vreg-3p3", 0,       0,       0,     false},
{NULL, "vreg-1p8", 1800000, 1800000, 14000, true},   <-- required
{NULL, "vreg-0p9", 1000000, 1000000, 40000, true},   <-- required
{NULL, "vreg-cx",  0,       0,       0,     false},
```

`kona-pcie.dtsi:47-49`：
```
vreg-1p8-supply = <&pm8150_l9>;      /* 1.8 V */
vreg-0p9-supply = <&pm8150_l5>;      /* 0.9 V */
vreg-cx-supply  = <&VDD_CX_LEVEL>;
```

**这两路是 `required = true`**，vendor 在碰任何 `0x60000000` 之前就把它们打开了。

### 四、我们从未开过任何轨（事实）

`grep pm8150|l9|l5|vreg|regulator|rpmh` 在 `alioth_pcie.c` 全文 → **0 命中**。
`NEW/UniXNU/drivers/` 里也**没有任何 SPMI/RPMh 稳压器驱动**
（只有 Adreno、ArmCpu、GopFramebuffer、QcomGeniSpi、RamBlock 等）。

**所以本树根本没有「打开 PMIC 轨」的能力。** 这是 9 条里唯一一条**我们完全不具备机制**的。

### 五、为什么这能解释「DWC 全挂、parf 可读」

- `0x01c00000`（parf/phy）在 **CX 域**，CX 已由 GPU 那条线证明可上电；
- `0x60000000`（DWC 的 dm_core/elbi/iatu/conf）需要 **1.8 V + 0.9 V 两路 PMIC 轨**。
  轨没开 → DWC 的 slave 端口没有供电 → **AXI 访问不返回**，正是我们测到的形状。

**这与「parf 可读、DWC 全挂」完全一致，而且它不需要「某片地址特别」这种说法。**

### 六、条件表（第 9 条）

| # | 解释 | 状态 |
| --- | --- | --- |
| 1 | SMMU SID | 排除 |
| 2 | VA 记账错 | 排除 |
| 3 | 映射属性错 | 排除 |
| 4 | 映射未建立/静默失败 | 排除 |
| 5 | 没上电（控制器 GDSC） | 排除（wifi7） |
| 6 | 没投票（GCC 时钟） | 排除（wifi5/7） |
| 7 | 漏了 vendor 的 dm_core 写 | 排除（wifi9） |
| 8 | 「只是某段地址」 | 排除（wifi10：conf 也挂） |
| 9 | **PMIC 稳压器未开（1.8 V / 0.9 V）** | **未测，且本树无此机制** |

**注意措辞**：这 9 条是「已想到的解释」的列表，不是「全部解释」。第 9 条本身就不在原 8 条里。

## 2026-10-31 05:05：这两路轨是 **RPMh** 稳压器，不是 SPMI LDO

离线核对。这条纠正了一个正要采用的错误方法。

### 一、`pm8150_l5` / `pm8150_l9` 是 RPMh 节点（事实）

`kona-regulators.dtsi`：

```
186  rpmh-regulator-ldoa5 {
195      L5A: pm8150_l5: regulator-pm8150-l5 {
             qcom,set = <RPMH_REGULATOR_SET_ALL>;
             regulator-min-microvolt = <880000>;
             ...
262  rpmh-regulator-ldoa9 {
271      L9A: pm8150_l9: regulator-pm8150-l9 {
             qcom,set = <RPMH_REGULATOR_SET_ALL>;
             regulator-min-microvolt = <1200000>;
             qcom,proxy-consumer-enable;
```

**节点名是 `rpmh-regulator-ldoa5` / `ldoa9`，`qcom,set = RPMH_REGULATOR_SET_ALL`。**
它们是 **RPMh 稳压器**（RSC 命令通道），**不是 SPMI LDO**。

### 二、所以「用 ButtonProbe 的 SPMI read8 读 LDO 状态」这条方法不适用

SPMI LDO（`qcom_spmi-regulator.c`）是另一套机制：每个 LDO 是 SPMI 的一个 peripheral，
用 APID + 寄存器偏移读写。**`pm8150_l5`/`l9` 不走那条路。**
用 SPMI 去读它们会读到一个不存在的 peripheral。

### 三、本树已有的 RPMh 能力（事实）

- `GpuProbe.cpp:482`：`ml_io_map(0x18220000, 0x10000)` —— **已经映射了 RPMh 的 DRV TCS**，
  并在 `:467` 的注释里写明「through active TCS 0 of DRV 2（apps_rsc drv-2 at 0x18220000）」。
- 启动日志每轮都打印 `rsc=0x18220000 tcs=0x18220d00 DRV=2 ...`，说明这条通道在内核里可读。

**所以「给 RPMh 投一票」这件事，本树有可复用的基础**（GpuProbe 做过）。

### 四、`BootLog.c` 里已经有 `[wifi-pmic]` / `[wifi-rails]` / `[wifi-1v8]` 的**读取端**

```
BootLog.c:736-750
  if (&native_wifi_pmic && native_wifi_pmic[0]) {
      "[wifi-pmic] arb=%x apids=%u regs=%u/%u gpio3=%x before=... write=%x after=..."
      "[wifi-rails] cmd-db addresses ldob15 smpg6 smpg4 smpe4 smpg5 smpg2: ..."
      "[wifi-rails] enabled-by-pmic-read=%x looked-at=%x last-wait-ms=%u total-ms=%u"
  if (&native_wifi_ldo && ...) {
      "[wifi-1v8] before: ..."
```

**但 `native_wifi_pmic[472]` 与 `native_wifi_ldo` 全树只有读取端，没有任何写入端**
（`grep` 除了 `BootLog.c` 的读和 `BootProgressStubs.c` 的弱定义，零命中）。
它来自提交 `4fd6373`（14:32，早期 stage1.5）。

**所以这些行是别人留下的空壳：基础设施的接口定义好了，实现从未写。**
这对本轮是好消息——**输出的形状已经定了**（`enabled-by-pmic-read`、`cmd-db addresses`
这些字段名说明设计者已经想过要读什么）。

### 五、下一步的正确形状

不是「用 SPMI 读 LDO」，而是：

1. **用 RPMh 的 cmd-db 查这两路轨的地址**（`[wifi-rails]` 那行要打印的正是
   `cmd-db addresses ldob15 smpg6 ...`——设计者已经知道要查 cmd-db）。
2. **读 RPMh 的 TCS 状态**判断它们当前有没有被投票（GpuProbe 的 DRV 2 TCS 0 通道可复用）。
3. 若没投 → 才谈怎么投。

**这三步都不需要新写 SPMI 驱动，而且第 2 步可以直接复用 GpuProbe 已经验证过的映射。**

### 六、`[wifi-pmic]` 空壳的来源查清了（补充）

上面说这些行「是别人留下的空壳」。查清了来源：

`BootLog.c` 来自 **sheng 板原样搬来**（提交 `4fd6373` 的说明：
「BootLog.c comes from the sheng board unchanged」）。那些 `[wifi-pmic]` / `[wifi-rails]` /
`[wifi-1v8]` 行是 **sheng 的**，写入端在：

```
NEW/UniXNU-sheng/package/WifiProbe.cpp:55   volatile uint32_t native_wifi_pmic[24 + 2*2*112];
NEW/UniXNU-sheng/package/WifiProbe.cpp:290  constexpr Rail kRails[] = {
                                              {"smpg5", 850, 0x6a7}, {"ldob15", 1800, 0},
                                              {"smpg2", 850, 0x69e}, {"smpe4", 950, 0x4a4},
                                              {"smpg4", 1300, 0x6a4}, {"smpg6", 1860, 0x6aa}};
NEW/UniXNU-sheng/package/WifiProbe.cpp:321  qcom_rpmh_active_request(&layout, tcs, &address, &value, 1, writes, 16);
NEW/UniXNU-sheng/package/WifiProbe.cpp:325  qcom_rpmh_finish(&layout, tcs, writes, 16);
```

**而 sheng 走的就是 RPMh**，不是 SPMI——`qcom_rpmh_active_request` / `qcom_rpmh_finish`
在 `NEW/UniXNU-sheng/package/QcomRpmh.c:63,86` 有完整实现（`AdspProbe.cpp` 也在用）。

### 七、这意味着本板有两条可复用路径

| 路径 | 位置 | 用途 |
| --- | --- | --- |
| **RPMh 投票** | `NEW/UniXNU-sheng/package/QcomRpmh.c` | 可直接参考实现（sheng 用它在 SM8550 上投 `ldob15` 等六路） |
| **RPMh TCS 映射** | `NEW/UniXNU-alioth/package/GpuProbe.cpp:482` | 本板已验证 `0x18220000` 可映射（DRV 2 TCS 0） |
| **输出形状** | `NEW/UniXNU-alioth/package/BootLog.c:736-750` | `[wifi-pmic]` 等行的字段已定，写端接上即可 |

**但 sheng 的 `kRails` 是 **SM8550/PMK8550 的**（`smpg5`、`ldob15`、`smpe4` 等），
**本板是 PM8150**，需要的是 `pm8150_l5`（0.9 V）与 `pm8150_l9`（1.8 V）。
所以**不能直接抄 `kRails`**，要按本板的 `kona-regulators.dtsi` 重新查这两路的 cmd-db 地址。

### 八、下一步

1. **查 `pm8150_l5` / `pm8150_l9` 在 cmd-db 里的地址**（`[wifi-rails]` 那行要打印的正是这个形状）。
   cmd-db 是 `/chosen` 或 smem 里的一张表，`BootLog.c:742` 的字段名说明设计者知道该查它。
2. **读它们当前的投票状态**（RPMh TCS，复用 GpuProbe 的映射）。
3. 若没投 → 用 `QcomRpmh.c` 的形状投这两路。

### 九、`[472]` 那个尺寸解码：它是 sheng 的 **SPMI 扫描**布局，不是 RPMh

有人提出「472 大到足够装一整套寄存器快照，说明设计者想到了完整方案」。**对，但那方案是 SPMI 的，不是 RPMh 的。**

`sheng/package/WifiProbe.cpp:55`：
```c
volatile uint32_t native_wifi_pmic[24 + 2 * 2 * 112];      /* = 472 */
```

布局：
```
[0..23]     header（arb 版本、apids、扫描到的 reg 数、gpio3、before/after 头、cmd-db 地址…）
[24..247]   before 扫描：112 个寄存器 × 2 个字（ppid/type + st/en/v）
[248..471]  after  扫描：同样的 112 × 2
```

`BootLog.c:846-852` 的读法确认了这一点：
```c
const uint32_t k0 = native_wifi_pmic[24 + 2 * i], v0 = native_wifi_pmic[25 + 2 * i];
if (i < na && native_wifi_pmic[248 + 2 * i] == k0) v1 = native_wifi_pmic[249 + 2 * i];
snprintf(line, ..., "[wifi-reg] ppid=%03x type=%x/%x before st=%02x en=%02x v=%02x%02x after st=%02x en=%02x", ...);
```

**每个寄存器按 PPID 定位、读 status/enable/voltage** —— 这是 SPMI 的读法
（`ppid = sid << 8 | (peripheral >> 8)`，见 `ButtonProbe.cpp:21-22` 的注释）。

### 十、所以这整套空壳是 sheng 的 SPMI 方案，不是本板要用的

- `[wifi-rails] cmd-db addresses ldob15 smpg6 smpg4 smpe4 smpg5 smpg2` 那六个名字是
  **sheng 的 PMK8550 轨**（`WifiProbe.cpp:290` 的 `kRails`），不是本板 PM8150 的。
- `[wifi-reg] ppid=...` 是 SPMI 的 per-register 读法。
- `[wifi-pcie] pipe-halt=... perst=...` 是 sheng 的 PCIe 探针（SM8550 的）。

**结论：这些行是 sheng 整套 WiFi 探针的输出接口，随 BootLog.c 一起搬过来，
本板没有对应的写入端，也不该照抄它们的语义。**

### 十一、对本板的实际意义

好消息：**BootLog 已经有地方放本板自己的 WiFi 读数**（`[wifi-pmic]` 等行格式已定），
只要写端填 `native_wifi_pmic[]` 即可。

但**不能沿用 sheng 的 SPMI 语义**：
- 本板这两路轨是 **RPMh**（`rpmh-regulator-ldoa5/9`），不是 SPMI peripheral；
- 本板要的 cmd-db 地址是 **PM8150 的 L5/L9**，不是 `ldob15/smpg5/...`。

**所以要么新写一个 RPMh 版的写端（复用 `QcomRpmh.c` 与 `GpuProbe.cpp:482` 的映射），
要么先不接这些行、用 `[PCIE]` 自己的行输出。**

## 2026-10-31 05:25：这两路轨的 cmd-db 名字是 `ldoa5` 与 `ldoa9`

离线核对。这是「查 cmd-db 地址」那一步的输入，现在确定了。

### 一、名字怎么来的（事实）

`kona-regulators.dtsi`（启动 DTB 里也一样）：

```
rpmh-regulator-ldoa5 {
    compatible = "qcom,rpmh-vrm-regulator";
    qcom,resource-name = "ldoa5";
    qcom,regulator-type = "pmic5-ldo";
    L5A: pm8150_l5: regulator-pm8150-l5 { regulator-name = "pm8150_l5"; ... }

rpmh-regulator-ldoa9 {
    compatible = "qcom,rpmh-vrm-regulator";
    qcom,resource-name = "ldoa9";
    ...
    L9A: pm8150_l9: regulator-pm8150-l9 { regulator-name = "pm8150_l9"; ... }
```

**本板走的是 vendor 驱动 `drivers/regulator/rpmh-regulator.c`**（不是 mainline 的
`qcom-rpmh-regulator.c`）。两者在名字来源上不同：

| | mainline `qcom-rpmh-regulator.c` | vendor `rpmh-regulator.c` |
| --- | --- | --- |
| 名字来源 | `qcom,pmic-id` + 格式串（`ldo%s5`） | **直接用 `qcom,resource-name`** |
| 是否要求 `pmic-id` | 是（`of_property_read_string` 失败即报错） | **不要求**（全文 `grep qcom,pmic-id` = 0） |

启动 DTB 里 `qcom,pmic-id` **出现 0 次**——所以如果按 mainline 的读法会直接失败，
而 vendor 的读法只用 `qcom,resource-name`，两个节点都给了。

**所以 cmd-db 的查找名就是 `"ldoa5"` 与 `"ldoa9"`。**

### 二、cmd-db 的查法本树已有（事实）

`GpuProbe.cpp`：
```
:183  alignas(8) uint8_t gCmdDb[GPB_CMD_DB_BYTES];
:184  gpb_cmd_db_entry gDbFound[8];
:187  void commandDb() { ... gpb_cmd_db_find(gCmdDb, sizeof gCmdDb, names[i], &found[i]); ... }
```
它已经用 `gpb_cmd_db_find` 查过 `gfx.lvl`、`mx.lvl`、`SH0`、`MC0`、`ACV` 等八个名字，
并把结果写进 `CMDDB ... name:address/aux` 行。

**所以查 `ldoa5` / `ldoa9` 的地址，只需要在 `commandDb()` 的 `names[]` 里加两项**，
或者在本文件里复用同一个 `gpb_cmd_db_find`。

### 三、下一步

1. **在 `alioth_pcie.c` 里复用 cmd-db 查找**（或读 `GpuProbe` 已打印的 `CMDDB` 行——
   如果那两路已经在里面，直接看日志即可）。
2. 拿到地址后，**用 RPMh TCS 读它的当前值**（`QcomRpmh.c` 的 `qcom_rpmh_active_request`
   是「写」的形状；读要走 observer 或 `RPMH_REGULATOR_SET_ACTIVE` 的当前请求）。
3. 若没投 → 用 `QcomRpmh.c` 的形状投。

**注意第 2 步的一个细节**：RPMh 的「读当前电压」不是直接读 TCS——TCS 是请求队列。
真正读要经过 RSC 的 observer 通道或 cmd-db 的 aux 数据。这一点在写读探针前要确认。

## 2026-10-31 05:30：`QcomRscLayout` 的校准已解决——GpuProbe 的偏移与本板一致，且实测跑通过

有人提醒「`QcomRscLayout` 是 sheng 的 SM8550 形状，偏移可能不对」。**查清了：它不按 SoC，按 RSC 的版本号；而本板是 v2，GpuProbe 已在本板实测用同一组偏移。**

### 一、`qcom_rsc_layout` 只按版本号选，不按 SoC（事实）

`NEW/UniXNU-sheng/package/QcomRpmh.c:53-57`：

```c
QcomRscLayout qcom_rsc_layout(uint32_t drv_id)
{
    if (((drv_id >> 16) & 0xff) >= 3)
        return (QcomRscLayout){672, 24, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c};
    return     (QcomRscLayout){672, 20, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38};
}
```

**输入只有 `drv_id`，字段是 `tcs_stride, cmd_stride, irq_status, irq_clear, wait_for_cmpl,
control, status, cmd_enable, cmd_msgid, cmd_addr, cmd_data`。** 版本 ≥ 3 用第一套，否则第二套。
**没有 SoC 相关的东西。**

### 二、本板的偏移已由 GpuProbe 实测确定（事实）

`GpuProbe.cpp:488-519` 用的常量：

```
kTcsBase = 0xd00, kTcsStride = 672, kCmdStride = 20
control      = 0x14      (w(tcs(id, 0x14), ...) 四次：AMC 关、关、开、触发)
cmd_enable   = 0x1C
cmd_msgid    = 0x30      (+ kCmdStride * c)
cmd_addr     = 0x34
cmd_data     = 0x38
irq_status   = 0x04      (kTcsBase + 0x04)
irq_clear    = 0x08      (kTcsBase + 0x08)
status       = 0x3C
```

**与 `qcom_rsc_layout` 的 v2 那套逐项比对：**

| 字段 | GpuProbe | `qcom_rsc_layout` v2 | 相同 |
| --- | --- | --- | --- |
| `tcs_stride` | `0x2a0` (672) | `0x2a0` | 是 |
| `cmd_stride` | `0x14` (20) | `0x14` | 是 |
| `irq_status` | `0x04` | `0x04` | 是 |
| `irq_clear` | `0x08` | `0x08` | 是 |
| `wait_for_cmpl` | `0x10` | `0x10` | 是 |
| `control` | `0x14` | `0x14` | 是 |
| `status` | `0x18` | `0x18` | 是 |
| `cmd_enable` | `0x1c` | `0x1c` | 是 |
| `cmd_msgid` | `0x30` | `0x30` | 是 |
| `cmd_addr` | `0x34` | `0x34` | 是 |
| `cmd_data` | `0x38` | `0x38` | 是 |

**11 项全同。** 而 GpuProbe 的 `rpmhVoteDdr()` 在这块板上**实测跑通过**
（`GPUROW` 里的 `DDRV tcs ... polls ... cmd status ...` 行，以及 GpuProbe 注释里记的
「all three completed in 0.3 ms」）。

### 三、结论

**本板的 RSC 是 v2 布局，`QcomRscLayout` 的 v2 那套在本板成立，而且有实测背书。**
`sheng` 的 `QcomRpmh.c` 可以直接复用——只要调用 `qcom_rsc_layout(drv_id)` 而不是硬编码 v3。

**这正是那个「零件适配」的答案**：不是「sheng 的偏移不能用」，而是
「`qcom_rsc_layout` 已经处理了两种布局，本板落在 v2 那支」。

### 四、`kTcsBase = 0xd00` 的来源

GpuProbe 的注释（`:467`）：「apps_rsc drv-2 at 0x18220000, qcom,tcs-offset 0xd00,
tcs-config ACTIVE 2, SLEEP 3, WAKE 3, CONTROL 1」。这个 `0xd00` 来自 DT 的
`qcom,tcs-offset`，是本板的值，不是抄来的。

**核对方法**：读启动 DTB 的 `apps_rsc` 节点确认 `qcom,tcs-offset` 是 `0xd00`。

### 五、`0xd00` 与 `drv-id 2` 已用启动 DTB 核实（事实）

```
apps_rsc: rsc@18200000 {
    compatible = "qcom,rpmh-rsc";
    reg = <0x18200000 0x10000 0x18210000 0x10000 0x18220000 0x10000>;
    reg-names = "drv-0", "drv-1", "drv-2";
    qcom,tcs-offset = <0xd00>;
    qcom,drv-id = <0x02>;
    qcom,tcs-config = <0x02 0x02 0x00 0x03 0x01 0x03 0x03 0x01>;
```

- `qcom,tcs-offset = 0xd00` —— 与 GpuProbe 的 `kTcsBase` 一致
- `qcom,drv-id = 2` —— 与 `ml_io_map(0x18220000, ...)`（drv-2 的基址）一致
- `qcom,tcs-config = <0x02 0x02 0x00 0x03 0x01 0x03 0x03 0x01>` —— 即
  **ACTIVE 2, SLEEP 3, WAKE 3, CONTROL 1**（每对是 `<mode count>`），
  与 GpuProbe 注释里的「tcs-config ACTIVE 2, SLEEP 3, WAKE 3, CONTROL 1」逐项一致

**三项全部对上，而且都来自本板的 DT，不是抄的。**

### 六、所以「零件适配」这条已答完

| 零件 | 是否适配本板 | 依据 |
| --- | --- | --- |
| `QcomRscLayout` 的 11 个偏移 | **适配（v2 那支）** | GpuProbe 在本板实测跑通，11 项全同 |
| `kTcsBase = 0xd00` | **适配** | 启动 DTB `qcom,tcs-offset = 0xd00` |
| DRV 基址 `0x18220000` | **适配** | 启动 DTB `drv-2`，GpuProbe 已映射 |
| TCS 配置（ACTIVE 2） | **适配** | 启动 DTB `qcom,tcs-config` 第一对 `0x02 0x02` |
| `kRails`（sheng 的六路轨名） | **不适配** | 那是 PMK8550 的，本板要 `ldoa5`/`ldoa9` |
| `[472]` 数组布局 | **不适配** | 那是 SPMI 扫描的，本板是 RPMh |

**所以：机制（RPMh + RSC 布局）适配；数据（轨名、数组语义）不适配。**
这个区分比笼统的「sheng 的东西不能抄」精确——**要抄的是机制，要重查的是数据。**

## 2026-10-31 05:35：PMIC 的实际输出电压在 APSS 侧读不到——只能实验验证

离线核对。这条改变了判据的性质。

### 一、`get_voltage` 读的是软件缓存，不是硬件（事实）

`rpmh-regulator.c:1021-1030`：
```c
static int rpmh_regulator_vrm_get_voltage(struct regulator_dev *rdev)
{
	struct rpmh_vreg *vreg = rdev_get_drvdata(rdev);
	uv = vreg->req.reg[RPMH_REGULATOR_REG_VRM_VOLTAGE] * 1000;
	if (uv == 0) uv = VOLTAGE_UNKNOWN;
	return uv;
}
```

`req.reg[]` 是**内核在 `set_voltage` 时写进去的缓存**（`:842-843`：
`old_value = vreg->req.reg[reg_index]; vreg->req.reg[reg_index] = value;`）。

`is_enabled` 同样（`:891`：`return !!vreg->req.reg[RPMH_REGULATOR_REG_ENABLE];`）。

**所以 Linux 自己也读不到 PMIC 的实际输出——它只知道「我请求过什么」。**

### 二、`cmd_db_read_aux_data` 读的是等级表，不是状态（事实）

`rpmh_regulator_load_arc_level_mapping`（`:1277-1310`）用它读 **ARC level mapping**
（电压等级 ↔ ARC 编码的常量映射表），**只在驱动初始化时调一次**，与「当前开没开」无关。

### 三、PM8150 的 LDO 在 SPMI 上不存在（事实）

`pm8150.dtsi` 的 `qcom,pm8150@0`（`compatible = "qcom,spmi-pmic"`, SID 0）子节点只有：

```
qcom,power-on@800      (qpnp-power-on)
                       (spmi-temp-alarm)
                       (spmi-clkdiv)
                       (spmi-sdam)
                       (spmi-gpio)
                       (pm8941-rtc)
                       (spmi-adc5)
                       (adc-tm5)
```

**没有 `qcom,spmi-regulator` 节点，也没有 LDO peripheral。**
L5/L9 只作为 `rpmh-regulator-ldoa5/9` 存在，走 RPMh。

**所以「绕开 RPMh、用 SPMI 直接读 PMIC」这条路不存在**——那两个 LDO 在 SPMI 上没有暴露。

### 四、结论：这一条的性质与前八条不同

| 前八条 | 第九条 |
| --- | --- |
| 「某个条件没满足」——可以查、可以读 | **「无法观测，只能实验」** |

**APSS 侧没有任何路径能读到「这两路轨现在实际输出了多少伏」或「开没开」。**
能做的只有：

1. **投一票，然后看 `0x60000000` 能不能读** —— 用结果验证，不是用状态验证。
2. 或者先查「bootloader 有没有投过」——但那只在 bootloader 自己的日志里，
   `fastboot getvar` 被禁用，这条路也断了。

### 五、所以下一步的现实形状

**判据必须从「先确认再动手」改成「投一票，看窗口是否变得可读」。**

这是一个**有明确因果假设的实验**（轨没开 → DWC 无电 → AXI 不返回），不是盲投：
- 若投后 `0x60000000` 可读 → **根因确定**，且整条 PCIe 路线打开
- 若投后仍挂 → 轨不是原因，回到「更上游」或「地址/门控」

**风险**：投 RPMh 是**写**操作。它作用于 PMIC 的稳压器，不是 PCIe 寄存器。
GpuProbe 的 `rpmhVoteDdr()` 已经在这块板上投过 DDR 相关的 BCM，**机制本身验证过**。
但投 LDO 会**改变供电状态**——如果电压值写错，可能损坏硬件。

**所以投之前必须：**
1. 用 cmd-db 拿到 `ldoa5`/`ldoa9` 的地址（**不猜地址**）
2. 用 `cmd_db_read_aux_data` 读到它们的**等级表**，确认要写的值是表里的合法项
3. 电压按 DT：`pm8150_l5 = 880000 µV`、`pm8150_l9 = 1200000 µV`
4. 先投**一路**（`ldoa5` 0.9 V），看窗口有没有变化——单变量

## 2026-10-31 05:42：两路轨都在 cmd-db 里，地址拿到

镜像 `boot-wifi12.img` sha256 `b828a934…`，内核 `682d8a9a…`，`pcie=12 pcie_skip=32`，
无 bt/bl/screenshot/mobile-graphics-load。248 行 PCIE（`tmp/console-wifi12r1.log`）。

### 一、结果（事实）

```
PCIE 12: the command database at 0x80860000: version 0x1 magic 0xc0330db (expected magic 0xc0330db)
PCIE 12: ldoa5: address 0x41800, aux 4 bytes (1 levels of 4)
PCIE 12: ldoa5: aux 1
PCIE 12: ldoa9: address 0x41b00, aux 4 bytes (1 levels of 4)
PCIE 12: ldoa9: aux 1
PCIE 12: step 13 is read only; no rail has been voted
```

- **cmd-db magic 正确**（`0xc0330db`），version `0x1` —— 库是好的
- **`ldoa5` 地址 `0x41800`**，aux 1 个 32 位值 = `1`
- **`ldoa9` 地址 `0x41b00`**，aux 1 个 32 位值 = `1`

### 二、判据落点

| 判据 | 结果 |
| --- | --- |
| 两路不在 cmd-db | 否 —— **两路都在** |
| aux 长度 = 0 | 否 —— **各 1 个等级** |
| aux 长度 > 0 | **是** —— 地址与等级表都有 |

**所以「RPMh 不认识这两路」这条排除。** 地址拿到了，这是投票的前提。

### 三、aux = 1 的含义（要看清，别读错）

`QcomRpmh.c:43-51`：
```c
int qcom_rpmh_level_index(const QcomRpmhResource *r, uint32_t level, uint32_t *index)
{
    if (!r->level_count) return QcomRpmhNoLevel;
    if (!level) {*index = 0; return QcomRpmhOk;}
    for (unsigned i = 0; i < r->level_count; ++i)
        if (r->levels[i] >= level) {*index = i; return QcomRpmhOk;}
    *index = r->level_count - 1;
    return QcomRpmhOk;
}
```

`aux = 1` 意味着**只有一个等级项**，其值 `levels[0] = 1`。

- `level = 0` → index 0（关）
- `level >= 1` → index 0（因为 `levels[0] = 1 >= level`）

**所以对这个 rail，投票只有「0 = 关」与「1 = 开」两档**——它不是一个可调电压的 VRM，
而是一个**开关式的 ARC 资源**。

**这与 `pm8150_l5`/`l9` 是固定电压（880000 / 1200000 µV，DT 里 min = max）完全一致**：
电压由 DT 定死，RPMh 这一侧只管「开/关」和 ARC 等级，不管具体伏数。

### 四、这对投票意味着什么

投票时写的是 `(index, 地址)` 这一对，**不是电压**：

```
qcom_rpmh_active_request(layout, tcs, {0x41800, 0x41b00}, {1, 1}, 2, writes, cap)
```

`values` 是 **ARC 等级索引**，不是 µV。**`aux` 表告诉我合法索引只有 0 和 1，所以写 1 是表里有的值。**

**这正是那道「防写错」的闸**：如果 aux 表里有 5 档而写 7，就越界；这里只有 1 档，写 1 是唯一合法值。

### 五、下一步

1. **投 `ldoa5` 与 `ldoa9`**（地址已定，值 = 1，机制已有 `QcomRpmh.c` + GpuProbe 验证的 RSC 布局）
2. 投完立刻读 `0x60000000`（dm_core 的第一个访问）—— **这一读就是判据**
3. 若可读 → 第 9 条成立，整条 PCIe 打开；若仍挂 → 轨不是原因

**风险说明**：这是第一次对 PMIC 轨投票。值 = 1 是 aux 表唯一合法项，电压由 DT 固定（不是我们选的），
所以「写坏电压」的风险很低——**我们投的是「开」，不是「设为某个伏数」。**

### 六、本轮没自回（事实，与读数无关）

`round-wifi12.out`：`DID NOT SELF-RETURN within 240s -- needs a power cycle`。

现场状态：板子不在 fastboot、不在 adb；`/dev/tty.usbmodem14101` 存在但**读 0 字节**（6 秒窗口）。

**关键：这一轮与前面几轮不同** —— PCIE 的 step 12 是**正常跑完**的（`step 12: done`，第 311 行），
之后还有 40 行 `TOUCH` 输出（第 349-351 行），**说明 boot 越过了 step 12 进入用户态**。
所以不是「PCIe 步骤挂死」导致的不自回。

**这与 wifi3/wifi5/wifi7 的「自回」形成一个未解释的差异。** 候选：
1. 这次 boot 在用户态某个地方停住了（与 PCIe 无关）
2. `TOUCH` 行之后的内核状态有问题

**这一条不在本轮判据范围内，但要记下来——它是第 10 条未知。**

### 七、下一步

读数已到手（地址 `0x41800`/`0x41b00`，aux 1 档）。**投票前需要板子恢复。**
投票的形状：

```
qcom_rpmh_active_request(&layout, tcs, {0x41800, 0x41b00}, {1, 1}, 2, writes, cap)
```

`values = {1, 1}` 是 **ARC 等级索引**，不是电压（aux 表只有一档，1 是唯一合法值）。
投完立刻读 dm_core `0x60000000` —— 那一读就是判据。

### 八、`GPB_CMD_DB_BASE` 不是「GPU 的常量」——它就是 RPMh 的 cmd-db（事实）

有人提醒：「`GPB_CMD_DB_BASE` 来自 `GpuPowerBoard.h`，那是 GPU 的板级常量；如果 RPMh 的 cmd-db 是另一块内存，读到的就是错的表。」

**核了，是同一块。**

本板启动 DTB 里：
```
cmd_db: reserved-memory {
    reg = <0x00 0x80860000 0x00 0x20000>;
    compatible = "qcom,cmd-db";
    no-map;
};
```

而 `GpuPowerBoard.h:346-347`：
```c
#define GPB_CMD_DB_BASE 0x80860000ULL
#define GPB_CMD_DB_BYTES 0x20000u
```

**基址与大小逐字一致。** 而且源码链也对上：`kona.dtsi:393` 的
`cmd_db: reserved-memory@80860000 { reg = <0x0 0x80860000 0x0 0x20000>; compatible = "qcom,cmd-db"; }`。

**vendor 的 `cmd-db.c:402` 是 `memremap(rmem->base, rmem->size, MEMREMAP_WB)`，
`rmem` 来自这个 reserved-memory 节点**——所以 Linux 读的也是同一块。

### 九、而且本轮实测已经证明它是对的（事实）

`PCIE 12` 打出的那一行：

```
the command database at 0x80860000: version 0x1 magic 0xc0330db (expected magic 0xc0330db)
```

- **magic 正确**（`0xc0330db`，与 `GpuPowerBoard.h:348` 的 `GPB_CMD_DB_MAGIC` 一致）
- **version 0x1** 是合法值
- **而且查到了 `ldoa5` / `ldoa9`** —— 如果读的是错的表，这两个名字不可能在里面

**所以「读到错的表」这条排除。** 实测比源码推断更硬：表是对的，因为它答出了两个只有 RPMh 的 cmd-db 才有的名字。

### 十、这两路是 **VRM** 类型，寄存器是「电压/使能/模式」——aux 表对它们无意义（重要更正）

上一节写「aux = 1 意味着只有一档，投票只能 0 或 1」。**那个解读错了。** 查源码后更正：

`kona-regulators.dtsi` / 启动 DTB：
```
rpmh-regulator-ldoa5 {
    compatible = "qcom,rpmh-vrm-regulator";     <-- VRM，不是 ARC
    qcom,resource-name = "ldoa5";
    qcom,regulator-type = "pmic5-ldo";
```

而 `rpmh-regulator.c:1926-1928`：
```c
if (aggr_vreg->regulator_type == RPMH_REGULATOR_TYPE_ARC) {
    rc = rpmh_regulator_load_arc_level_mapping(aggr_vreg);    /* 只有 ARC 才读 aux 表 */
```

**`ldoa5`/`ldoa9` 是 VRM，所以 `load_arc_level_mapping` 对它们从不调用。**
那个 `aux = 1` 的值对 VRM 不构成「等级表」——**它是别的语义，不能拿它当「合法档位」。**

### 十一、VRM 的寄存器布局（事实）

`rpmh-regulator.c:87-99`：
```c
enum rpmh_regulator_reg_index {
    RPMH_REGULATOR_REG_VRM_VOLTAGE = 0,
    RPMH_REGULATOR_REG_VRM_ENABLE  = 1,
    RPMH_REGULATOR_REG_VRM_MODE    = 2,
    RPMH_REGULATOR_REG_VRM_HEADROOM= 3,
    RPMH_REGULATOR_REG_VRM_MAX     = 4,
};
```

而写命令的构造（`:771-776`）：
```c
cmd[j].addr = aggr_vreg->addr + i * 4;    /* i 是上面的索引 */
cmd[j].data = req_active.reg[i];
```

**所以对 `ldoa5`（地址 `0x41800`）：**

| 偏移 | 寄存器 | 含义 |
| --- | --- | --- |
| `0x41800 + 0` = `0x41800` | VRM_VOLTAGE | **电压，单位 mV**（`get_voltage` 里 `* 1000` 得 µV） |
| `0x41800 + 4` = `0x41804` | VRM_ENABLE | **使能** |
| `0x41800 + 8` = `0x41808` | VRM_MODE | 模式（LPM/HPM） |
| `0x41800 + 12` = `0x4180c` | VRM_HEADROOM | 余量 |

`ldoa9` 同理，基址 `0x41b00`。

### 十二、所以正确的投票形状

**不是「ARC 等级索引」，而是分别写三个寄存器：**

```
电压：  addr 0x41800, data 880      (mV，DT 里 pm8150_l5 = 880000 µV)
使能：  addr 0x41804, data 1
模式：  addr 0x41808, data <HPM 索引>
```

`ldoa9`：`0x41b00` = 1200 mV（`pm8150_l9 = 1200000 µV`），`0x41b04` = 1（使能）。

**这三个值分别写，不是像 GpuProbe 的 DDR 票那样写一个「BCM 位图」值。**
**两种语义完全不同**——`GpuProbe` 的 `top = (1<<30)|(1<<29)|(0x3fff<<14)|0x3fff` 是 BCM 的编码，
不能用在 VRM 上。

### 十三、这对风险的影响

**比上一节估的高。** 因为现在要写**具体电压值**（880 mV / 1200 mV），不是「开/关」。
写错电压真的可能损坏硬件。

**所以投票前必须再确认两件事：**
1. **`req_active.reg[]` 的单位确实是 mV**（`get_voltage` 里 `* 1000` 暗示是 mV，
   但 `set_voltage` 那边要再看一眼确认写入时是否也按 mV）
2. **`mode` 的值**（HPM 在 RPMh 的编码是多少）——`qcom,init-mode = <RPMH_REGULATOR_MODE_HPM>` 在 DT 里有

**这两条都是零成本的源码核对。在没核对前不投。**

### 十四、投票所需的三个值全部核实（事实）

| 项 | 值 | 出处 |
| --- | --- | --- |
| 电压单位 | **mV** | `rpmh-regulator.c:986` `mv = DIV_ROUND_UP(min_uv, 1000)` |
| `ldoa5` 电压 | **880** | DT `pm8150_l5`：min = max = 880000 µV |
| `ldoa9` 电压 | **1200** | DT `pm8150_l9`：min = max = 1200000 µV |
| 使能值 | **1** | `RPMH_REGULATOR_REG_VRM_ENABLE` 的 `req_active.reg[1]`，`regulator_enable` 置 1 |
| 模式值 | **7** | `rpmh-regulator.c:281` `RPMH_REGULATOR_MODE_PMIC5_LDO_HPM = 7`；DT `qcom,init-mode = HPM` |

**注意模式值不是枚举 `RPMH_REGULATOR_MODE_HPM`（=4），而是 `pmic_mode` 字段（=7）。**
`set_mode_index`（`:1049-1050`）写的是 `aggr_vreg->mode[mode_index].pmic_mode`，
而 `pmic5_ldo` 的 HPM 项是 `RPMH_REGULATOR_MODE_PMIC5_LDO_HPM = 7`（`:367`）。

**所以完整的投票是：**

```
ldoa5 (0x41800):  +0 = 880 (mV)   +4 = 1 (enable)   +8 = 7 (HPM)
ldoa9 (0x41b00):  +0 = 1200 (mV)  +4 = 1 (enable)   +8 = 7 (HPM)
```

共 6 条命令，超过 `qcom_rpmh_active_request` 的 `count <= 4` 上限。

**所以必须分两次投**（每次 3 条：一个 rail 的 voltage/enable/mode），
或者只投一个 rail（3 条）作为单变量——**后者更符合「一次只改一个变量」**。

### 十五、推荐的投票顺序

1. **只投 `ldoa5`（0.9 V）**：`{0x41800:880, 0x41804:1, 0x41808:7}` —— 3 条，单变量
2. 投完立刻读 dm_core `0x60000000`
3. 若可读 → 根因确定；若仍挂 → 再投 `ldoa9`（1.2 V），或说明轨不是原因

**为什么先 `ldoa5`**：0.9 V 是较低的电压，即使写错，后果比 1.2 V 轻。而且 DT 里它是 `vreg-0p9`，
与 `vreg-1p8` 是两条独立轨——**先开一路，看窗口有没有变化，是最小实验。**

## 2026-10-31 05:59：投票序列的 dry run —— 打印出来了，与实际核对一致，**没有写**

镜像 `boot-wifi13.img` sha256 `9b52f1a2…`，内核 `06be6f31…`，`pcie=13 pcie_skip=32 pcie_rail=1`。
253 行 PCIE（`tmp/console-wifi13r1.log`）。

### 一、step 13 的 dry run 输出（事实）

```
PCIE 13: the first write this file makes to a power rail: ldoa5 (pm8150_l5, 0.9 V) through RPMh
PCIE 13: command 1: address 0x41800, data 880 (voltage 880 mV)
PCIE 13: command 2: address 0x41804, data 1 (enable 1)
PCIE 13: command 3: address 0x41808, data 7 (mode 7 (pmic5-ldo HPM))
PCIE 13: pcie_rail=1: dry run, no write made; pcie_rail=2 makes these three
PCIE 13: step 13: done
```

**三条与实际核对表逐项一致：**

| cmd | 地址 | 数据 | 表 |
| --- | --- | --- | --- |
| 1 | `0x41800` | 880 | 一致 |
| 2 | `0x41804` | 1 | 一致 |
| 3 | `0x41808` | 7 | 一致 |

**`pcie_rail=1` 是 dry run 分支：打印后直接返回，**没有调用 `ml_io_map`、没有写任何寄存器**。
`step 13: done` 是正常的，不是失败。

### 二、门控设计（事实）

```c
PE_parse_boot_argn("pcie_rail", &mode, sizeof(mode));
if (mode == 0) return true;          /* 没给参数：什么都不做，什么都不打印 */
... 打印三条命令 ...
if (mode != 2) { 打印 "dry run, no write"; return true; }   /* pcie_rail=1 走这里 */
... 只有 pcie_rail=2 才 ml_io_map + 写 ...
```

**三重保险**：无参数不动作、`=1` 只打印、`=2` 才写。

### 三、RSC 偏移已核对（离线计算）

```
TCS 0: control=0xd14  cmd_enable=0xd1c  status=0xd18
       cmd0: msgid=0xd30  addr=0xd34  data=0xd38
```

与 GpuProbe 在本板实测跑通的 `kTcsBase=0xd00 + 0x14/0x1c/0x30/0x34/0x38` **逐项一致**。

### 四、下一步（需要用户在场）

**`pcie_rail=2` 才是真正的写。** 判据在写之后：

- DWC 窗口（`0x60000000`）读得出 → **轨是原因**，整条 PCIe 打开
- 仍挂 → **轨不是原因**（投完之后轨一定开着），第 9 条排除

**无论哪种结果都排除一条解释。** 这不是「赌能不能通」。
