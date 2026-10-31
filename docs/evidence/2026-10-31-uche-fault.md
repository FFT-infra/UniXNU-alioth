# `GPUROW28` 的 UCHE OOB：第三次提交读到未映射的 IOVA

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
