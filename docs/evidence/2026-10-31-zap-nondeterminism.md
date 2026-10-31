# `zapLoad` 是**非确定性**的：同一镜像三轮三种结果

2026-10-31，同一块板（`9c6a8eb6`）、**同一个镜像文件** `tmp/boot-a6xx2.img`
（191,264,768 字节，05:07 构建后未改），三轮读数：

| 轮次 | 时刻 | GPUROW 序列 | 结论 |
| --- | --- | --- | --- |
| `a6xx2`（05:18） | 05:18 | `0 3 6 7 13 14 15 17 18 19 20` | 停在 `CPSTATE`，没有 ZAP 行 |
| `a6xx2`（07:48） | 07:48 | `0 3 6 7 13 14 15 17 18 19 20 **21 22 23 24 25 28**` | **ZAP ok，一路走到 FAULTS** |
| `zaprepro2`（08:36） | 08:36 | `0 3 6 7 13 14 15` 然后 14/15 循环 13 次 | **连 17（GX 上电）都没到** |

## 07:48 那轮是最好的一次

```
GPUROW21 ZAP ok step 7 init 0/0 mem 0/0 auth 0/0 resumes 0 ms 7/0/0 phdrs 3 reloc 1 span 1000 secure 0 meta d5eac000
GPUROW22 IRQ attach 0 readback a002 router 0 count 1>2 nop fence 1 seen 00100000 status now 00000000 masked 0
GPUROW23 PUBLISH ok submit 0 timer 0 publish 0 | uc 36/36 first 0 at 0 cpumap 0 | rounds 4 4
GPUROW24 METAL ok: accelerator registered (IOGPUFamily loaded, class size 136), Turnip options nocb
GPUROW25 LIVE irq4 last100000 seen100000 m0 | sub 2/2 t2 p0 | uc 2/2 | bo 0/0 r0 s0 | busy 0
GPUROW28 FAULTS cr0 260406 gfsr 0 syn 0/0/0 | cb0 fsr 400 syn 40c0e far 5e0be55794f5
```

这一轮**比 10-29 的基线还远**：10-29 只到 `GPUROW21`（`docs/evidence/2026-10-29-gpu-gx-and-context.md:61`），
07:48 走到了 `GPUROW28`，包括 publish（`uc 36/36`）、Metal 注册（`IOGPUFamily loaded`）和故障表。

但 `GPUROW25 LIVE` 的后续样本是坏的：`sub 3/2`（已提交 3、完成 2）、
`busy` 从 994 涨到 1000、`t` 从 2 涨到 1,034,562 —— **栅栏等待超时，GPU 不前进**。
`GPUROW28` 的 `cb0 fsr 400` 是 UCHE 的 OOB，与 `far 5e0be55794f5` 一起，指向同一处地址。

## 08:36 那轮为什么停在 15

`GPUROW14 LOAD` 与 `GPUROW15 HFI` 各重复 13 次，间隔是探针自己的 2 秒行周期。
`GPUROW15` 是 HFI 阶段，**在 `zapLoad` 之前**；`GPUROW17`（GX 上电）没有出现。
所以这一轮探针**根本没走到 GPU context**，不是 `zapLoad` 失败。

## 不能从这三轮得出的结论

- **不能说「`zapLoad` 卡在 `scmCall`」**——07:48 那一轮它成功了（`init 0/0 mem 0/0 auth 0/0`，
  7 ms）。`PLAN-EXEC §E0` 写的是 05:18 那一轮的读数，已过时。
- **不能说「`meta` 地址不对」**——07:48 是 `d5eac000`，10-29 是 `13ed13000`，两次都成功。
  地址不同不构成故障。
- **不能说 08:36 那轮是「卡在 15」**——它只是没走远；「为什么没走远」尚未解释。

## 下一轮该问的

三轮差异在**探针走到哪一步**，不在某个固定位置卡住。所以下一个单变量不是改 `scmCall`，
而是先让**同一镜像重复 N 次**，统计 `GPUROW` 能到达的最远行：

| 次数 | 最远行 | 备注 |
| --- | --- | --- |
| 1 | 20 | 05:18 |
| 2 | 28 | 07:48 |
| 3 | 15 | 08:36 |

若 3 次里只有 1 次到 21 以上，说明这是**竞态**（与开机时序、USB 枚举、电源轨稳定时间有关），
而不是代码缺陷。下一步应该记录每次的 **boot_id + 探针启动时刻**，找相关性。

---

## 第二轮：第四次复跑（08:51），把「三种结果」变成分布

| 轮次 | 时刻 | 最远行 | 行集合 | `GPUROW15` 次数 |
| --- | --- | --- | --- | --- |
| 1 | 05:18 | 20 | `0 3 6 7 13 14 15 17 18 19 20` | — |
| 2 | 07:48 | **28** | `… 15 17 18 19 20 21 22 23 24 25 28` | 18 |
| 3 | 08:36 | 15 | `0 3 6 7 13 14 15` | 13 |
| 4 | 08:51 | 15 | `0 3 6 7 13 14 15` | 14 |

（工具：`tmp/probe-depth.sh <console.log>`，把「最远行」变成一个可重复的读数。）

### 关键：15 那两轮不是「没走远」，是**卡在 `gxOn()` 内部**

`GPUROW17` 是 `gxOn()` 的第一行输出（`GpuProbe.cpp:530-545`）。15 那两轮
**完全没有 `GPUROW17`**，而 `GPUROW15` 各重复 13/14 次——那是探针的 2 秒行周期，
说明 `gxOn()` 被调用后一直没有返回。

`gxOn()` 的调用条件（`GpuProbe.cpp:1533`）：`gMode >= 8 && gColdOk`。
`gColdOk` 来自 GMU 冷启动（`:450`），而 `GPUROW14 LOAD` 显示 `phase 8`、`ready 11`、
`hfi 1`、`cm3 1`——**冷启动是成功的**。所以 `gxOn()` 确实被调用了。

`A650GmuPowerGx`（`kits/pipa/A650Gmu.c:334-356`）里有三个会阻塞的点：

| 点 | 位置 | 阻塞方式 |
| --- | --- | --- |
| `AdrenoHfiExchange(hfi, 30, vote, 3, 1000, …)` | `:345` | HFI 30 号命令，超时 1000 ms |
| `OOB_GPU_SET_REQUEST` 写 + `GMU2HOST_INTR_INFO` 轮询 | `:346-351` | 最多 100 次 × 100 µs = 10 ms |
| `wait_us(100)` 每次轮询 | `:350` | 同上 |

**三个点都不是无限循环**，所以「卡住」不是死等——更可能是 `wait_us` 本身没返回
（`d->services.wait_us` 是板级注入的函数指针），或者 `AdrenoHfiExchange` 内部在等
一个永不到来的 HFI 回复。

### 这一轮推翻的与确立的

**推翻**：`PLAN-EXEC §E0` 与 `progress/2026-10-31.md §5.4` 的「探针卡在 `zapLoad` 的
`scmCall`」。07:48 那轮 `zapLoad` 成功（`init 0/0 mem 0/0 auth 0/0`，7 ms）。

**确立**：四轮里两轮停在 `gxOn()`，一轮停在 `GPUROW20`（`CPSTATE` 之后），
一轮走到 `GPUROW28`。**停点不同，不是同一个故障。**

### 下一轮要的单变量

不要改 `scmCall`，也不要在 `A650GmuPowerGx` 里猜。**先加三行打印**，让停点自己说话：

1. `gxOn()` 入口一行：`GPUROW16 GX enter perf <n> cold <ok>`
2. `AdrenoHfiExchange` 返回后一行：`GPUROW16a GX vote status <s> err <e> polls <n>`
3. `A650GmuPowerGx` 返回后一行：`GPUROW17 …`（已有）

三行都在 `GpuProbe.cpp` 里加，`kRowGx` 是同一个槽，用不同的 `native_gpu_step`
区分。**只改打印，不改逻辑**——这样下一轮无论停在哪，都能知道它过了哪一步。
