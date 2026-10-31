# `GPUROW28` 的 UCHE OOB：第三次提交读到未映射的 IOVA

> **2026-10-31 16:20 更正：标题与下文的「UCHE OOB」判据不成立。**
>
> `cb0 fsr 0x400` 不是故障。SMMU-500 的 `CBn_FSR` 里 bits 10:9 是 FORMAT（AArch64 stage-1 =
> `0b10`），`FSR_FAULT = FSR_MULTI|FSR_SS|FSR_UUT|FSR_EF|FSR_PF|FSR_TF|FSR_IGN = 0xc00001fe`
> （`reference/k40-kernel/drivers/iommu/arm-smmu-regs.h:253-258`），不含 bit 10。厂商的 fault
> handler 正是用这个掩码判「非故障」：
> `if (!(fsr & FSR_FAULT)) { ret = IRQ_NONE; ... }`（`arm-smmu.c:2058`）。
> 我们自己的驱动也写了同一件事：`tmp/parity-src/NativeAdrenoGpuContext.cpp:204-205`
> 在 `endFault` 里把 FSR 的 bits 10:9 掩掉（`out[0]&=~0x600`），注释写明「0x400: AArch64,
> not a fault」。
>
> 因此 `far 0x5e09e75794f5` 是 **FAR 在 FSR 没有故障位时的未定义回读，不能当作 GPU 访问过
> 94 TiB 的证据**。它跨四次启动几乎不变（后三字节恒为 `94f5`），也印证是陈旧/未定义值。
>
> 真正的信号是 `RBBM_INT_0_STATUS` 的 **bit 9 = CP_HW_ERROR**：失效轮读到
> `irq115008 last200 seen900200`，`0x900200` = bit9 + bit20(CP_FENCE) + bit23(HANG)，
> 而 `last200` 说明 CP 硬件错误是最后一次中断。`bit24 (UCHE_OOB_ACCESS)` 与
> `bit25 (UCHE_TRAP)` 从未出现；探针里 `gpu regs oob` 的转储也从未打印。
>
> 另外，探针的 `gpuIrq`（`package/GpuProbe.cpp:1104`）只读/清 RBBM 状态并计数，**从不读
> CP 的三个错误寄存器**；只有 `hangRows()` 会读，而它只在「fence 不动且 CP 的 RPTR/IB1/REM
> 也不动」时才触发。失效轮里 CP 仍在动，所以 HANG 行没有出现——另外这两行（26/27）
> 也不在 `BootProgress.c` 的打印列表里，即使写了也不会送到 console。
>
> 所以本文下面的「94 TiB 的 IOVA」「窗口内未映射地址」是**从 FORMAT 字段推出来的，
> 全部撤回**；保留原文只作为历史。


2026-10-31 的 `zap7`/`zap7b`/`zap7c` 三轮（同一镜像，三次一致）都停在同一个地方：

```
GPUROW25 LIVE irq115017 last200 seen900200 m1 | sub 3/2 t1019609 p0 | uc 5/3 | bo 0/0 r0 s0 | busy 999
GPUROW28 FAULTS cr0 260406 gfsr 0 syn 0/0/0 | cb0 fsr 400 syn 40c0e far 5e09e75794f5
```

## 读数本身是对的

`smmuRead(bank0 + 0x64)` 在前、`smmuRead(bank0 + 0x60)` 在后，拼成 `high<<32 | low`。
Linux 对同一个寄存器用的是 `readq_relaxed(cb_base + ARM_SMMU_CB_FAR)`
（`reference/k40-kernel/drivers/iommu/arm-smmu.c:1888`、`:2081`），`ARM_SMMU_CB_FAR = 0x60`
（`arm-smmu-regs.h:180`）。小端机上 `readq` 读 `0x60`–`0x67`，低字在 `0x60`、高字在 `0x64`。
**探针的拼法与 Linux 相同，所以 `far = 0x5e09e*` 是真实读数，不是拼错。**

## 这个地址是什么

| 量 | 值 |
| --- | --- |
| `far` | `0x5e09e75794f5` = **94.0 TiB** |
| GPU IOVA 上限（`kDeviceInfo`） | `0x1fffffffff000` = 512 TiB |
| 客户窗口 slot0 起始 | 512 MiB（`reference.md §31`） |
| slot0 窗口 | 170 MiB |

**94 TiB 在 IOVA 上限之内，但比任何客户窗口大六个数量级。** 所以它不是某个 BO 的地址，
是**一个从未被映射的 IOVA**——GPU 读到它，SMMU 报 OOB（`fsr 400` 是 UCHE 的
`UCHE_INVALID_OP` 类），提交停在那里。

## 什么时候发生

`GPUROW25` 的序列说明了：

| 阶段 | `sub` | `busy` | 含义 |
| --- | --- | --- | --- |
| 内核自测 | `2/2` | `0` | 两次提交都完成 |
| 用户态之后 | `3/2` | `999`–`1000` | **第三次提交永不完成** |

`uc 5/3`：5 个客户端、3 个成功。`GPUROW23` 是 `PUBLISH ok … rounds 4 4`——发布阶段本身是好的。

**所以故障在「Metal 的第一个真实提交」，不在发布、不在 zap、不在 GMU。**

## 与已知结论的关系

`reference.md §31` 记的是「客户端窗口」的读数口径（`peak_mib` 是绝对地址、`GPUSLOT hi` 是相对值），
以及 slot0 的 967 MiB 约束。**本条不是那个问题**：那是窗口容量，这里是提交里出现了一个
窗口之外的地址。

`reference.md §33.5` 记的是 BO 池（768 MiB，峰值 95% 满）。parity 分支把固定池去掉了
（`porting.toml` 与 `2026-10-31.md §3.5`），**所以 BO 池不是这里的约束**——但「某个 BO 的
IOVA 被算到了 94 TiB」与「池子不够」是两类问题，不能混。

## 下一轮（离线先做，不需要板子）

1. **94 TiB 从哪来**：`0x5e09e*` 与 `kDeviceInfo` 的哪个字段有关？先把 `NativeAdrenoUserClient`
   的 IOVA 分配点列出来（`windowStart_`、`windowPeak_`、每次映射的 iova），看有没有一处会把
   地址加到一个过大的基址上。
2. **`fsr 400` 与 `syn 40c0e`**：`40c0e` 的低位是 `0x0e`（UCHE 的 OOB 子类），高位 `40c0`
   可能是 cache 操作类型。对照 `a6xx.xml.h` 的 `UCHE_INVALID_OP` 位域。
3. **`GPUROW23` 的 `rounds 4 4`**：四次提交都成功。**故障只出现在第五次**（用户态的第一笔）。
   所以先查「用户态提交与内核提交在 IB 构造上的差别」，而不是查 SMMU 配置。

**判据**：在 `NativeAdrenoUserClient` 的提交路径里加一行打印，记录每次提交的 IB 首地址与
长度，看第三次的 IB 里有没有 94 TiB 的地址。

---

## 补：94 TiB 为什么没被窗口检查拒绝

`NativeAdrenoUserClient::setScope`（`parity-src/NativeAdrenoUserClient.cpp:140-150`）把窗口设成
`[GpuClientVaStart, 2^input_bits)`：

```
GpuClientVaStart = 0x100000000      // 4 GiB, msm ADRENO_VM_START (NativeAdrenoGpuContext.h:21)
window           = 2^input_bits - GpuClientVaStart   // NativeAdrenoPublish.cpp:120
```

| `input_bits` | 窗口上限 | 94 TiB 在内？ |
| --- | --- | --- |
| 36 | 64 GiB | 否 |
| 40 | 1 TiB | 否 |
| 44 | 16 TiB | 否 |
| **48** | **256 TiB** | **是** |

所以 `input_bits = 48` 时，**94 TiB 是窗口内的合法 IOVA**，`setScope` 接受、`mapBo` 的
`if(window_bytes_ && …)` 也放行（`:312`）。

**结论：这不是「窗口太小」或「地址越界」，而是「窗口内的一个地址从未被映射」。**
GPU 读到它，SMMU 报 OOB。窗口检查按设计工作——它管的是**范围**，不管**是否映射**。

## 与 BO 计数对不上的地方

`GPUROW25` 的 `bo %u/%u` 取 `native_adreno_bo_memory[4]` 与 `[5]`（`GpuProbe.cpp:1452`），
用户态之后一直是 `bo 0/0`。**若用户确实映射了 BO，这两个计数不该是 0。**
两种可能：
1. parity 去掉固定池之后，这两个计数不再更新（读数是旧的）；
2. 用户态在提交之前没有成功映射任何 BO——那么 94 TiB 的读就是**从一张空表开始的第一次读**。

**下一轮先判这一条**：`mapBo` 每次成功时加一行打印（handle、iova、bytes），看用户态那几次
映射到了哪些地址。若那几次的地址都不在 94 TiB 附近，说明 94 TiB 来自别处（例如 IB 里的
一个错误指针），而不是一次映射。

---

## 关键一轮：新插件下故障不变

`plug1`（11:30 构建，`tmp/boot-plug1.img`）是**第一个用 parity 源重建的插件**：

| 输入 | 时间 | 来源 |
| --- | --- | --- |
| Turnip `libvulkan_freedreno.dylib` | 10:54 | `build-turnip-darwin.py --target ios` → PASS，17,881,712 B |
| bridge `libadreno_metal_bridge.a` | 11:07 | `build-adreno-metal-bridge.py --target ios` → Finished |
| 插件 `NativeAdrenoMetal` | 11:17 | `build-adreno-metal-plugin.py`，18,554,832 B，CDHash `72bc50c2` |
| addon | `gpu-parity4.gpuaddon` | 9 files，12,033,112 B |

（插件编译时用 `CCC_OVERRIDE_OPTIONS='+-Wno-unused-function'` 绕过一个**上游**的
`#if` 不一致，见 `docs/evidence/2026-10-31-plugin-unused-function.md`；脚本与源都没改。）

上板结果：

```
GPUROW0  GPU m12 …
GPUROW25 LIVE … | sub 3/2 t1029788 p0 | uc 5/3 | busy 999
GPUROW28 FAULTS cr0 260406 gfsr 0 syn 0/0/0 | cb0 fsr 400 syn 40c0e far 5e09e55794f5
```

**与旧插件的三轮（`zap7`/`zap7b`/`zap7c`）逐字相同**：`sub 3/2`、`busy 999`、`far 5e09e55794f5`。

## 所以

**94 TiB 这个故障与插件版本无关。** 它不来自 10-30 的旧插件，也不来自 parity 的新插件。
它在**驱动侧或 Metal 侧**（`NativeAdrenoUserClient` 的 IOVA 分配，或 Metal 发出的 IB）。

这排除了一个先前无法排除的变量（`docs/evidence/2026-10-31-stale-metal-addon.md` 里的
「无法归因到 parity 的插件或旧插件」）。

## 下一轮的目标（离线）

按 `docs/evidence/2026-10-31-uche-fault.md` 前面列的：在 `NativeAdrenoUserClient` 的提交
路径里记录每次提交的 IB 首地址与长度，看第三次的 IB 里有没有 94 TiB 的地址。

**这是驱动侧改动，要改 `tmp/parity-src`、重新钉 `feeds.toml`、重建、上板**——完整一轮。

---

## 2026-10-31 16:20：换判据，读 CP 的错误寄存器

### 已有的 `[gpu stall]` 两行（零改动，`ibread3`）

`build/sheng-logs/ibread3-00000002041afeb6/latest.txt`：

```
[gpu stall] fence=5 n=22215 800005 0 0 0 0 0 8000430e 0 3d 3d 20004000 0 0 0 0 0 deafbead deadbead deadbead deadbead deadbead deadbead
[gpu stall] smmu 400 e75794f5 5e09 10c0e
[gpu stall] br ib1 @ffff0000: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
[gpu stall] bv ib1 @ffff0000: deadbeef ×16
[gpu stall] bv ib2 @ffff0000: deadbeef ×16
[gpu stall] handles job>=8 br=6 bv=0 bv2=0
```

- `smmu 400 e75794f5 5e09 10c0e` = FSR `0x400`（FORMAT，非故障）、FAR `0x5e09e75794f5`、
  FSYNR0 `0x10c0e`（bit4 WNR / bit5 PNU / bit6 IND 全 0，也不是故障类别）。
- **BR IB1 的 16 个字全是 0**，而 BV IB1/IB2 不可读（`deadbeef`）：CP 的 BR 取指落在
  一块零填充的内存上。这比 FAR 更有信息量——它说明「CP 执行的是 0 字」，与
  `CP_HW_ERROR` 同向。
- `handles job>=8 br=6 bv=0 bv2=0`：BR IB1 落在 handle 6 的映射里；BV 的两个 IB 地址
  没有对应 handle。

### 探针改动（`cperr1`/`cperr2`）

`package/GpuProbe.cpp`：

- `gpuIrq` 在 `RBBM_INT_0_STATUS & (1<<9)` 时按 `a6xx_cp_hw_err_irq`
  （`reference/k40-kernel/drivers/gpu/drm/msm/adreno/a6xx_gpu.c:533-583`）的顺序读并锁存
  `0x823 CP_INTERRUPT_STATUS`、`0x821 CP_HW_FAULT`、`0x824 CP_PROTECT_STATUS`；若
  `0x823` 的 bit0（opcode error）置位，再写 `0x908=1` 读 `0x909`（SQE stat）。
- 新增 `GPUROW31`（`kRowCpErr`），由心跳每 2 秒从锁存值格式化：标志位按厂商顺序
  `O U H P A V I`，寄存器保护状态带方向与地址。
- `native_gpu_probe` 在解析后把 `arg/parsed/gMode` 写进同一行（`GPUPARSE`）；CP 错误发生时
  该行被 `CPERR` 覆盖。

`package/BootProgress.c`：三处打印列表加入 `31`。

### 上板

| 轮次 | 镜像 | 结果 |
| --- | --- | --- |
| `cperr1` | `tmp/boot-cperr1.img`，sha `cf39e50f…`，kernel `319146cb…` | 板子自回；但 console 捕获挂在**过期节点**上，得到的是上一轮缓存的旧内核输出（`GPU m7`、内核版本 10-28），不是本轮 |
| `cperr2` | `tmp/boot-cperr2.img`，sha `c2ade3d4…` | 板子 240 秒未自回，`board-state` = `reader-or-userspace`；console 捕获未生成文件，进程随脚本退出 |
| `cperr2b`（断电后重跑） | 同 `cperr2` 镜像 | 同一形态，16:15 进用户态；`/dev/tty.usbmodem14101` 的 mtime 是 16:15:37，**早于本轮开始**，`console-capture.py` 一直判 stale，`tmp/console-cperr2b.log` 不存在。脚本在超时前被停掉，锁已清 |

### 这一轮暴露的脚本判据缺陷

`console-capture.py` 以「节点 mtime 晚于本轮开始」判新鲜。这颗板子**启动后不再重新枚举串口**：
节点在启动时出现一次，mtime 就是那一刻，之后一直不变。所以这个判据在这颗板上**恒为假**，
捕获器永远等不到「新鲜」节点——cperr1/cperr2/cperr2b 三轮都是这样，不是硬件。

改法（下一轮改脚本，不要动正在跑的）：

- 新鲜度改成「节点存在，且本轮开始后能打开并读到数据」，打不开再重试；或
- 不依赖 console：把 GPU 行写进持久日志。

后者已经落地：`package/BootLog.c` 增加 `native_gpu_rows` 弱引用，把第 0/23/25/31 行
写进日志文本块（提交 `542c438`）。持久日志是这颗板上唯一能跨轮稳定读出的通道。


`cperr2` 运行中直接读活着的 console 节点，得到：

```
GPUARG call 1 pipa-gpu=12 parsed=1 gMode=0
```

**所以本轮确实是 mode 12，`m7` 是过期 console 的伪读数。** 之后节点静默（用户态停住，
与 `2026-10-31-userspace-stall.md` 同形），无法再取 `GPUROW31`。

### 下一轮

板子需要断电后重跑（锁已在 `tmp/board.lock` 标成 dead，重跑前要删）。判据：

- `GPUROW31 CPERR irq … st … <flags> hw … prot … sqe …` 出现，且 `GPUROW23` 的 `uc b>0`；
- flags 为 `O`/`I` → 命令流非法（0 字 IB 与 BR IB1 全 0 一致）；
- flags 为 `P` → 寄存器保护，读 `prot` 的地址与方向；
- flags 为 `A` → CP 的 AHB 访问被中止，才回到 SMMU；
- flags 为 `H`/`U` → CP/ucode 硬件错误。

