# GPU 呈现：fix76 到 fix78 的读数与边界

2026-11-01。设备仓库 `NEW/UniXNU-alioth` @ `b4b8d850e019b9a5f0ea69687e71a2b633a5d3ab`，
基仓库 `NEW/UniXNU` @ `4f701809ef5ef10a187a9ecf57c312eb93c4b4fb`。
**真机读数，串口抓取。**

原始抓取：`tmp/console-fix76.log`、`tmp/console-fix77.log`、`tmp/console-fix78.log`。
持久日志：`NEW/UniXNU-alioth/build/sheng-logs/fix76-000000027f8a9640`、
`fix77-00000007989b8824`、`fix78-00000004c8dfbf38`。

**这两类日志不含同一批行。** `GPUSWAP`、`GPUFB`、`GPUREASONS`、`GPUFRAME`、`GPUOPEN`
经 `qcom_usb_debug_write` 只进串口；持久日志里有 `GPUROW*`、`[gpu clients]`、`[scanout wait]`、
`[fence wait]`。引用时必须写明是哪一个。

---

## 1. 三轮对比

| 读数 | fix76 | fix77 | fix78 |
| --- | --- | --- | --- |
| 提交 | `sub 25/25` | `sub 688/688` | `sub 1075/1075` |
| IB 指令流 | `ibs=163` | `ibs=7959` | `ibs=14157` |
| 环读写指针 | `rw 542/542` | `rw 268/268` | `rw a8e/a8e` |
| ROQ | `roqavail 0/0/0` | `0/0/0` | `0/0/0` |
| 呈现帧 | 7（停住） | 303 | 303 |
| 末态面板 | 26/1000 luma 5 | 710/1000 luma 65 | 336/1000 luma 73 |
| 峰值面板 | — | 981/1000 luma 237 | 987/1000 luma 243 |
| `gpuaddon` | `gpu-parity4` | `gpu-parity5` | `gpu-parity5` |
| `sheng-vsync` | 60 | 60 | 120 |

三轮都 `rounds 4 4`、`tgt badc0de feed0001 600d0003 deadbeef`、`rp 0`、`p0`。

---

## 2. fix76 在七帧之后停住

`tmp/console-fix76.log` 共 1186 行，末次 `GPUSWAP validate 7` 在第 935 行，
之后到 `TOUCHSTAT t=180s` 再没有增长。同期持久日志 `sub 25/25`、`[fence wait] calls=94 timeouts=28`。
`GPUFB` 末态 `7879e1af changes 1 (+0) nonblack 26/1000 luma 5` —— 面板 97% 是黑的。

所以 fix76 的呈现是**七帧后停住的近黑画面**，不是持续出帧。
后续 fix77（换 `gpu-parity5`）把帧数推到 303、面板推到 710/1000。

---

## 3. 三个字段的读法（三个都曾被读错）

### 3.1 `SWAP calls=N accepted=1` 里的 `accepted` 是布尔值

`BootProgress.c:728` 把 `native_legacy_swap_validation_result` 打在 `accepted=` 位置。
`LegacyDisplayProbe.cpp:344` 是 `native_legacy_swap_validation_result = accepted;` ——
**最后一次交换的判定**，不是计数。计数在 `GPUREASONS accepted N`。
`porting.toml:361` 已记录过这一点。

### 3.2 `GPUFRAME ... copied 0` 说明呈现走的是 GPU 路径

`copied` 是 `native_gop_transfers`，只在 `NativeGOPPresenter::transfer()` 加。
fix76/77/78 都是 `copied 0 / gpu offered N done N` —— 帧由 GPU 客户端复制进 GOP 内存，
CPU 拷贝路径一次都没走。

### 3.3 `[gpu objects] w158M` 是窗口末端，不是显存占用

`w` 是 `native_adreno_client_stats[5]` = 窗口映射的最高末端（MiB），
`NativeAdrenoUserClient.cpp:336`。`[gpu clients]` 里的 `4254M` 同理是 `windowPeak`。

---

## 4. `GPUROW26 HANG` 是按需打印

`GpuProbe.cpp:1617`：`if (st.submissions != st.completed && rptr == lastRptr && ib1 == lastIb1 && rem1 == lastRem1) hangRows(t);`
fix75 有这一行（`r4e/4e ib1 310000+0`），fix76 起没有 —— 因为 `sub 25/25` 没有挂起。
"没有这一行"是"没有挂起"，不是"这一行被修好了"。

---

## 5. 未解决的项（fix78 实测）

### 5.1 帧率不稳，末端塌陷

按 `TOUCHSTAT` 时间戳对齐 `GPUSWAP`：

| 区间 | 帧 | fps |
| --- | --- | --- |
| 60→75 s | 19 | 1.27 |
| 75→80 s | 67 | 13.40 |
| 80→90 s | 58 | 5.80 |
| 90→100 s | 142 | 14.20 |
| 100→120 s | 8 | **0.40** |

同期 `[fence wait] calls=4873 timeouts=2640`（54%）、`[scanout wait] calls=692 timeouts=181`。

### 5.2 AegirPoster 仍然崩溃

`CRASH p267 AegirPoster sig6`、`CRASH p269 AegirPoster sig6`。
`b0c316f` 想用 `lite_jobs` 禁用它，串是 `com.apple.AegirPoster`，
真实 label 是 `com.apple.NanoUniverse.AegirProxyApp.AegirPoster`（`msgbuf.txt` 原文）。
`KaleidoscopePoster` 同理：真名 `com.apple.WatchFacesWallpaperSupport.KaleidoscopePoster`。

### 5.3 窗口槽位不足

`[gpu windows] ... mount fail stage 1 st e00002d5 slot 0 n 2` —— 两次 `kIOReturnBusy`。
`GPUOPEN` 68 次里 52 次 resolver `e00002e2`（NoResources）、16 次 `e00002c7`（NotReady）。
驱动侧 grant 放行（`NativeAdrenoPublish.cpp:927`），但槽位是另一回事。

### 5.4 面板不收敛

`GPUFB` 序列 911 → 28 → 26 → 341 → 336 → **987** → 336，`changes 30`。
没有一次稳定。采样 `J120.png` 自身是 `nonblack 607/1000 luma 37`，与面板任何一帧都对不上。

---

## 6. 成立的结论（源码对主线已核）

1. `CP_APRIV_CNTL` (0x844) 写 `BIT(6)|BIT(5)|BIT(3)|BIT(2)|BIT(1)`，与
   `linux-mainline/drivers/gpu/drm/msm/adreno/a6xx_gpu.c:1420` 的 A6XX `hw_apriv` 分支逐位一致。
2. `CP_RB_CNTL` 不置 `NO_UPDATE`，与 `a6xx_gpu.c:1452` 的 `if (hw_apriv) write(MSM_GPU_RB_CNTL_DEFAULT)` 一致。
   A650 在 `a6xx_catalog.c:1056` 确有 `ADRENO_QUIRK_HAS_HW_APRIV`。
3. GPU 提交链在 fix78 推到 1075 笔全完成、环指针同步、零挂起。


---

## 7. 2026-11-02：DPU 实测与零拷贝的第一次上板

设备仓库 `NEW/UniXNU-alioth-gpu`，基仓库 `NEW/UniXNU`。原始抓取在
`build/sheng-logs/fix84-00000001ed13991b`、`fix85-000000010548800a`。

### 7.1 显示控制器实测（`native-dpu=1`，DpuProbe.cpp）

```
[dpu] st=1 hw=60000000 fb=09c000000
[dpu pipe] 0x5000(sm8250 vig0) size=0 src0=0 stride=0 fmt=0 unpack=0
[dpu pipe] 0x7000(sm8250 vig1) size=0 src0=0 stride=0 fmt=0 unpack=0
[dpu pipe] 0x4000(sm8550 vig0) size=9600438 src0=9c000000 stride=10e0 fmt=237ff unpack=3020001 LIVE
[dpu pipe] 0x6000(sm8550 vig1) size=0 src0=0 stride=0 fmt=0 unpack=0
```

**本板的 VIG 0 在 `0x4000`，不是设备树 `kona-sde.dtsi:110` 写的 `0x5000`。** 判据是三个寄存器自洽：

| 寄存器 | 值 | 解码 |
| --- | --- | --- |
| `SRC0_ADDR` | `0x9c000000` | 本板 GOP 帧缓冲地址（`GOP actual mapping ... physical=9c000000`） |
| `SRC_SIZE` | `0x9600438` | 宽 `0x960=2400`，高 `0x438=1080` |
| `SRC_YSTRIDE0` | `0x10e0` | 4320 字节/行 |

`0x5000` 全零，说明那是 SDE 目录的偏移，不是 MDP 块内的偏移。

### 7.2 固件的源格式就是 BGRA

`SRC_FORMAT = 0x237ff`，按 `dpu_hw_sspp.c:289-307` 的编码拆开：

| 位段 | 值 | 含义 |
| --- | --- | --- |
| `[3:0]` C0_G_Y | 15 | 8 位/通道 |
| `[5:4]` C2_R | 3 | |
| `[7:6]` C3_A | 3 | |
| `[9:11]` | 15 | bpp-1，即 4 字节/像素 |
| `[13:12]` | 3 | unpack_count-1，4 个通道 |
| `[18:17]` | 1 | fetch_planes = interleaved |

`SRC_UNPACK_PATTERN = 0x3020001` → `element[0..3] = 1,0,2,3` → 字节序 `C1,C0,C2,C3`，
配合 `C0=G, C1=B, C2=R, C3=A` 得 **B,G,R,A**。固件扫的就是 BGRA 8888，
与拷贝路径写入的格式一致。**通道顺序不是颜色问题的原因。**

### 7.3 之前一版测量工具的通道顺序是错的（已修）

`BootProgress.c` 的 `framebufferChecksum` 曾把 `pixel & 0xff` 当红、`>>16` 当蓝，
而扫描内存是 BGRA（固件 `SimpleFb.c:182` 声明 `PixelBlueGreenRedReserved8BitPerColor`）。
luma 的权重 299/114 因此被互换，任何非灰的画面读数都偏。`nonblack` 不受影响。
2026-11-02 已改正；此后 `GPUFB luma` 与之前的读数不可直接比较。

### 7.4 零拷贝第一次上板：整个系统停住

`fix85` 与 `fix84` 的 boot 参数只差 `sheng-dpu-scanout=2`：

| 轮次 | uptime | procs | SWAP | DPU 行 |
| --- | --- | --- | --- | --- |
| fix84（无零拷贝） | 178 s | 214 | 8 | 有 |
| fix85（零拷贝） | **18 s** | **1** | **0** | **无**（探针在第 20 s 才读） |

fix85 停在 `launchd` 起来之前（`p1 init status=0`），SMP 仍 `run=7 step7`，
`GPUROW3` 的 `gfsr 00000000`。所以不是 SMMU 报错，是显示 bank 的 stage-1 表一装上、
系统就停在这一步。零拷贝在当前形态下不可用。

**未解决**：`dpuEnable` 里 50 ms 的故障观察窗口在本板等于把系统挂死，
需要先证明显示 bank 能接受这张表（或证明它根本不该被写），再谈零拷贝。

---

## 8. 2026-11-02 第二轮：零拷贝三次失败，呈现不稳定

### 8.1 零拷贝三次上板全部失败

| 轮次 | 改动 | 结果 |
| --- | --- | --- |
| fix85 | 窗口建在 `hostStartQueue` 之前 | 18 s 停住，procs=1 |
| fix87 | 窗口移到 `hostStartQueue` 之后 | **178 s 正常**，窗口装成 `state=0x80000004`，但 SWAP 只有 7 |
| fix88 | 在 `vsyncTick`（工作循环）里调 `startDirect` | 18 s 停住（`addEventSource` 死锁） |
| fix89 | 改成定时器延迟重试 | 88 s，procs=197，SWAP=0 |
| fix90 | 加 stage 诊断 | **240 s 无响应，需断电** |

`state=0x80000004` 证明 stage-1 表**确实装成功了**：`smr=0x84020820`（VALID + ID 0x820）、
`s2cr=0x4`（`S2CR_TYPE_TRANS` + bank 4，按 arm-smmu.h:115-120）。所以不是 SMMU 配置错。

**结论：零拷贝在当前形态下不可用。** 显示驱动在 `start()` 时窗口还不存在（NativeUfs 建窗在根挂载之后），
而任何"稍后再试"的方案都要在工作循环里操作事件源，那会死锁。要真正做成，需要让 NativeUfs 在显示
启动之前建窗 —— 那是根启动的顺序改动，不是显示驱动能解决的。

### 8.2 `gpu offered` 不是稳定指标

同一个 `gpu-parity6.gpuaddon`、同一套 boot 参数：

| 轮次 | 显示代码 | gpu offered | GPUROW24 | backboardd |
| --- | --- | --- | --- | --- |
| fix86 | 有本地改动 | **102** | SELFTEST | **无** |
| fix91 | 有本地改动 + presenter 改动 | 0 | SELFTEST | 无 |
| fix92 | 只有显示改动 | 0 | SELFTEST | 无 |
| fix93 | **完全干净**（`git stash`） | 0 | METAL ok | 有 |

**fix86 是唯一 offered>0 的一轮，但它在 SELFTEST 模式下且没有 backboardd。**
干净代码的 fix93 是 `METAL ok` + backboardd + `offered 0`。
所以"`offered 102 done 61 fail 41`"不能当作正常呈现的证据，也不能当作基线。

### 8.3 本轮修好的真问题

1. **`framebufferChecksum` 的通道顺序**（BGRA 读成 RGB）已修，见 §7.3。
2. **DPU 探针**（`DpuProbe.cpp`）：VIG 基址、固件格式、stride、CTL 全部可读，一轮就能判定。
3. **`[direct scanout]` 行**：`native_legacy_direct[14]` 从写入起就没被打印过，现在有了。
4. **`native_ufs_dpu_state` 行**（`[dpu window]`）：窗口是否装成、装到哪个 bank，一次可见。
5. **`native_dpu_pipes/stats` 的类型不匹配**：stub 是 `uint64_t`、读者是 `uint32_t`，已删 stub、由 DpuProbe 定义。

### 8.4 构建陷阱（两次踩到，记录在案）

1. 手写的 `tmp/build-fix79.sh` 缺 `sync-tree-scripts.py` 与 pin 校验，导致 fix79 停 `step4` 被误判为
   policy 的错。**构建一律走 `tmp/build-parity.sh`。**
2. `NEW/UniXNU-alioth` 与 `NEW/UniXNU-alioth-gpu` 是两份，`build-parity.sh:42` 读的是后者。
   改了前者不生效 —— fix82/fix83 就是这么白跑的。

---

## 9. 2026-11-02 第三轮：按官方顺序启动，直通路径首次启用

### 9.1 改动：板级按依赖顺序启动（官方形态）

对照官方文档与本板代码后发现，本板**早就实现了官方要求的 late probe**：

- `docs/zh/porting/ufs.md` 第 5 步：更新存储部分、跑记录检查，**再继续显示与亮度**。
- `docs/zh/porting/drivers.md` 第 2 节：依赖顺序 2=存储、3=显示。
- `docs/zh/porting/drivers.md` 第 3 节第 1 条：**板级主动启动的服务需要在资源和 OS 依赖就绪后显式调用。**
- sheng 的 `LegacyDisplayProbe.cpp` `native_legacy_display_late_probe()` 就是那个显式序列：探针在前、显示在最后。

本板 `LegacyDisplayProbe.cpp:931` 已有同名函数（HID、GPU、背光、按钮、PCIe 都在里面串行启动），
缺的只是**把存储的显示窗口排进这个序列**。fix95 补上了：

1. `NativeUfs` 导出 `native_ufs_smmu_base()` 与 `native_ufs_dpu_window_build(smmu)`，
   两个都是 `extern "C"`、定义在匿名 namespace 之外（在里面会内链，链接器看不到）。
2. 板级 late probe 在 `display->start(nub)` **之前**调用它们。
3. **删掉** `NativeUfs` 里的 `native_legacy_direct_ready()` 反向回调 —— 存储组件不该知道显示存在。

### 9.2 结果：直通路径第一次真正启用

```
[dpu window] state=80000004 idr0=4c017e57 groups=87 valid=7 banks=74 | stream0 slot/bank=40004 smr=84020820 s2cr=4
[direct scanout] stage=4 flips=0 mapped=0 refused=0 recycle=0 noslot=0 boot=0 | ...
```

`stage=4` 是"mapped and taken"，即 `startDirect()` 走完了全部检查并 `directOn=true`。
fix87/fix89 都只到 stage 3（armed but waiting），fix95 是第一次到 4。板子 178 s 正常返回。

### 9.3 但 `flips=0`：没有帧可翻

`SWAP calls=0`、`DISPLAY ... owned=0`、`GPUFRAME ... gpu offered 0`。
合成器一次交换都没提交，所以直通路径没有可翻的帧。

这不是零拷贝的问题，是**独立的一层**：同一份代码、同一套参数下

| 轮次 | 零拷贝 | SWAP calls |
| --- | --- | --- |
| fix86 | 无 | 1067 |
| fix93 | 无（`git stash` 干净） | 0 |
| fix95 | 有（stage=4） | 0 |

fix86 的 1067 与 fix93/fix95 的 0 在同一代码基线上分别出现，说明交换的发生是环境或时序相关的，
不是代码路径决定的。**在弄清"为什么有时有交换、有时没有"之前，零拷贝的翻转无法验证。**

### 9.4 这一轮之后的代码状态

- 零拷贝：按官方依赖顺序接好，`sheng-dpu-scanout=2` 时启用，失败回退 GPU 复制（官方原文）。
- `NativeUfs`：不再反向调用显示；只导出数据与建窗入口。
- 诊断：`[dpu window]`、`[direct scanout] stage=`、`[dpu pipe]`、`[dpu ctl6]` 都在。

---

## 10. 2026-11-02 第四轮：`sheng-fb-bgra=1` 生效，白名单扩大

### 10.1 `Turnip options` 这一行曾误导判断

`GpuProbe.cpp` 的 `METAL ok` 行打印的 `turnip` 字段只来自 `pipa-turnip-debug`，默认 `"nocb"`，
**而插件实际用的 `TU_DEBUG` 来自另一条路径**：`NativeAdrenoMetalPlugin.mm:299`

```objc
setenv("TU_DEBUG", bootArgSet("sheng-fb-bgra=1") ? "sysmem,nocb" : "nocb", 0);
```

所以 fix96 传了 `sheng-fb-bgra=1` 却仍打印 `Turnip options nocb`，看日志像是没生效。
fix97 把 `sheng-fb-bgra` 也读进这个字段后，同一轮打印 `Turnip options sysmem,nocb`，
**证明参数确实传到了插件**。

### 10.2 白名单扩大生效

`GpuProbe.cpp:1468` 原本只写 `backboardd,iconservicesagent,Preferences`。
扩到 `CollectionsPoster,AegirPoster,KaleidoscopePoster,PhotosPosterProvider,PosterBoard,WidgetRenderer_Default,mediaserverd,SpringBoard` 后：

| 轮次 | `[gpu clients]` |
| --- | --- |
| fix95（旧） | kernel_task、backboardd、SpringBoard、WidgetRenderer_Defa |
| fix97（新） | 再加 mediaserverd、PhotosPosterProvide、PosterBoard、**CollectionsPoster** |

注意进程名上限：`native_sandbox_grant` 用 `char name[17]`，`proc_selfname` 截到 16 字符，
所以 `CollectionsPoster`（17 字符）在沙盒侧是 `CollectionsPoste`；补丁按前 16 字符比较，全名能匹配。

### 10.3 直通路径与 bgra 是两条独立开关

fix95 传 `sheng-dpu-scanout=2` → `stage=4`（启用）。
fix97 只传 `sheng-fb-bgra=1` → `stage=1`（没请求）。两者互不影响，构建时要都传才同时验证。

### 10.4 帧数与亮度必须同时报触摸

| 轮次 | `low` | SWAP | GPUFB |
| --- | --- | --- | --- |
| u276 | 791 | 728 | nonblack 1000/1000 luma 253 |
| fix96 | 123 | 37 | nonblack 173/1000 luma 31 |
| fix97 | **0** | 10 | nonblack 28/1000 luma 6 |

u276 的 1000/1000 是 `low=791`（有人一直在滑）。fix97 的 28/1000 是 `low=0`（没人碰），
按 `decisions/2026-10-30-unattended-decisions.md:324-362` 的纪律，**这不能判为故障**。
**亮度差异无法与触摸分离，颜色正确性在无人操作的一轮里测不出来。**

---

## 11. 2026-11-02 第五轮：直通翻转卡在 MDP 写入（已定位，未修复）

### 11.1 三轮对照把故障锁在寄存器写入

`[direct scanout]` 行在 fix100 加了 `branch=` 计数（进入 flip 分支的次数），在 fix101 加了
`native-dpu-skip=<bits>`（1 跳过 VIG stores，2 跳过 CTL flush，4 两者都跳）。
分成一个位掩码而不是三个开关，因为 `BOOT_LINE_LENGTH` 是 1024（`pexpert/arm64/boot.h:25`），
本镜像的命令行已经到 1015 字节，三个二十字节的开关会把尾部参数挤掉。

| 轮次 | MDP 写入 | 时长 | branch | flips | 结果 |
| --- | --- | --- | --- | --- | --- |
| fix100 | VIG + CTL（CTL 基址 0x2000） | 178 s | 1 | **0** | 活，但翻转没返回 |
| fix101 | 全跳过 | 178 s | 1 | **1** | 完成 |
| fix103 | 只 VIG，跳过 CTL | 20 s | — | — | 板子停机（`swaps 0`，与 flip 无关） |

fix100 与 fix101 只差"写不写寄存器"，`branch=1 mapped=1` 在两轮都成立，唯一变量是写入。
所以 **`flip()` 里的 MDP 寄存器写入会挂住调用**，不是分支、不是映射、不是 `directSlot`。

`mapped=1 flips=0` 这个曾经"代码上不可能"的组合由此解释通了：`directSlot` 返回后，
`flip()` 没有返回，下一行的 `native_legacy_direct[0]++` 从未执行。

### 11.2 基址仍未确定

VIG 基址 `0x4000` 是 DpuProbe 实测的（`src0=0x9c000000`、`size=0x9600438`、`stride=0x10e0` 三项自洽），
与 sheng 相同。CTL 基址试过 `0x2000`（设备树）与 `0x15000`（sheng），
但 `[dpu ctl6]` 的 6 个 CTL 的 layer0 与 flush 全读 0，**无法从读数区分哪个是对的**。

写入会挂起这一点说明：写入的地址落到了未上电或行为不同的寄存器区。读能返回 0 不代表写安全。

### 11.3 当前状态

- 不传 `sheng-dpu-scanout=2` 时，`startDirect()` 在 stage=1 就返回，**完全不碰 MDP 寄存器**。
  fix95/fix97 都是这种情况，178 s 正常。这条是官方文档要求的"默认关闭，失败回退 GPU 复制"。
- 传了 `sheng-dpu-scanout=2` 时，窗口能建、surface 能映射（`mapped=1`），但翻转会挂住。
  **在 MDP 写入被证明安全之前，直通路径不可用。**

### 11.4 下一步的假设：窗口 iova 落在 identity block 之外

探针读到固件的 VIG 正在扫描 `src0=0x9c000000`，即 GOP 物理地址。零拷贝把 `iova=0x10000000`
（`kDpuWindow`，slot 0）写进同一个寄存器，DPU 随即去那里取图。

显示 bank 的 stage-1 表里，identity block 覆盖 `0x80000000..0xffffffff`（`l1[2]` 与 `l1[3]`），
窗口在 `l1[128 + t]`（`0x10000000 >> 21 == 128`）。**`0x10000000` 不在 identity 覆盖范围内。**
如果 DPU 的取图请求没有真的经过 stage-1 翻译（stream 未匹配，或表未生效），它就把 `0x10000000`
当物理地址读；本板 RAM 从 `0x80000000` 起，`0x10000000` 无对应内存 → 总线 stall → CPU 卡在 flip 里。

这与三条读数一致：读同一批寄存器不卡（探针）、不写就不卡（fix101 `flips=1`）、写了就卡（fix100 `flips=0`）。

**下次上板的判据**（一次分开）：
1. flip 写回 `0x9c000000`（当前 GOP 地址，identity 覆盖范围内）。不卡 → 写本身安全，问题在翻译路径。
   卡 → 写这个块的任何地址都不安全，方向变成时钟/电源。
2. 若第 1 步不卡，再写 `0x10000000`。卡 → 确认是窗口 iova 的翻译问题，修法是让窗口落在
   identity 范围内，或证明 DPU 的 stream 走了 stage-1。

### 11.5 结论：MDP 寄存器在非安全世界不可写，零拷贝在本板不可用

`[dpu]` 行补上 dispcc 读数后（fix107，`st=1` 表示探针完整跑完）：

```
[dpu] st=1 hw=60000000 fb=09c000000 | dispcc ahb=1 mdp=221 (bit1 set = gated)
```

- `ahb=0x1`：bit0 CLK_ENABLE 置位、bit1 CLK_OFF 清零 → AHB 时钟在跑。
- `mdp=0x221`：bits 0/5/9，bit1 同样清零 → **MDP 时钟没有被 gate。**

**时钟不是原因。** 四条读数把范围收到一处：

| flip 的动作 | 结果 |
| --- | --- |
| 写窗口地址 `0x10000000` | 挂 |
| 写 GOP 地址 `0x9c000000`（identity 内） | 挂 |
| 写回读到的原值（硬件状态不变） | **挂** |
| 完全不写 | 完成（`flips=1`） |

读同一批寄存器成功（探针 `st=1`，`src0=0x9c000000` 等三项自洽），写则一律挂住。
读允许、写被拦，是安全世界保护的典型形态，而不是时钟或地址问题。

硬件侧的对应记录在 `kona-sde.dtsi`：

- `:220` `qcom,sde-secure-sid-mask = <0x4000821>` —— 显示控制器声明了安全 SID。
- `:282-287` `smmu_sde_sec_cb`，`qcom,iommu-vmid = <0xa>` —— 安全虚拟机 ID 0xa 的 context bank。

即本板的显示控制器由安全世界（bootloader / hypervisor）持有，非安全世界只能读它的状态。
写入被拦在总线上，表现为 CPU 卡在 store 指令里，与 fix100/104/105 完全一致。

**因此零拷贝在本板不可用，不是代码缺陷。** 这与官方 `display.md:65` 的定位一致：
"直接扫描输出为实验功能，默认关闭……任何失败都会回退到 GPU 复制。"

### 11.6 交付状态

- 不传 `sheng-dpu-scanout=2` 时，零拷贝完全不执行（`stage=1`），不碰任何 MDP 寄存器 —— 这是官方默认。
- `flip()` 保留 `native-dpu-skip` / `native-dpu-iova-gop` 两个实验开关，供将来在显示控制器被
  移出安全世界的固件上复现这条路径。
- 直通路径的其余部分（窗口建立、surface 映射、work loop 上的安全映射）已按官方依赖顺序接通，
  在 fix95 达到 `stage=4 mapped=1`，这部分与硬件保护无关，将来可直接复用。
