# 失聪核的 GIC 状态，以及睡在它上面的线程为什么永远不醒

2026-11-06 下午。设备仓库 `NEW/UniXNU-alioth`，ArmCpu `9750897`（逐核失聪记录、
redistributor 代读、逐核中断相位）。真机读数，USB 串口抓取，`fastboot boot`，不写槽。

| 镜像 | 与上一镜像的差别 | 原始抓取 |
| --- | --- | --- |
| `boot-w3.img` | `CORES` 行加 `wd=`/`skip=`；判定失聪需连续三次采样不动 | `tmp/console-w31219511.txt`、`tmp/console-w31227142.txt` |
| `boot-w4.img` | 加 `GICR` 行：失聪核的 `ISPENDR0`/`ISENABLER0`/`ISACTIVER0`/`WAKER` | `tmp/console-w41234361.txt`、`tmp/console-w41241592.txt` |
| `boot-w8.img` | `cpus=4`，只起小核簇 | `tmp/console-w81307361.txt` |
| `boot-qq1.img` | 11-05 21:22 的已知好镜像，按字节复跑 | `tmp/console-qq1r1256471.txt`、`tmp/console-qq1r1301382.txt` |

## 1. 失聪核的 GIC 帧（由健康核代读）

```
w41234361  GICR t=40s..190s deaf=40 c6=600/8040000/80000ff/0/0
w41241592  GICR t=50s..190s deaf=20 c5=500/8040000/80000ff/0/0
w81307361  GICR t=30s        deaf=04 c2=200/8040000/80000ff/0/0
```

格式 `c<N>=<GICR_TYPER 亲和值>/<ISPENDR0>/<ISENABLER0>/<ISACTIVER0>/<WAKER>`。三轮一致：

- bit 27（虚拟定时器 PPI）**pending**；
- bit 27 **使能**（`0x80000ff` 含 bit 0..7 与 27）；
- `ISACTIVER0 = 0`：这个中断**从未被取走**；
- `WAKER = 0`：redistributor 报告该核**清醒**。

所以不是 GIC 被关掉，也不是整簇掉电（`cpus=4` 时小核 c2 同样出现，排除大核簇假设）。

## 2. 睡在失聪核上的线程永远不醒

`SUPER` 行的 `tc=` 是 `native_touch_core`（触控循环最后运行的核）。四轮里 `tf` 冻住与
`tc` 等于失聪核完全同现：

| 轮次 | 失聪核 | tc | tf | sw/own |
| --- | --- | --- | --- | --- |
| `w31219511` | c5 | 5 | 839 冻 | 0/0 |
| `w31227142` | c5 | 4→6 | 25941 | 9/1 |
| `w41234361` | c6 | 6 | 840 冻 | 11/1 |
| `w41241592` | c5 | 1 | 13211 | 1/1 |

机制在源码里：本板 `cpu_idle_latency = 0`，`ml_delay_should_spin` 恒 FALSE
（`osfmk/arm64/machine_routines.c:2484`），`IOSleep` 因此走
`_clock_delay_until_deadline_with_leeway` 的 `assert_wait_deadline` + `thread_block`
（`osfmk/kern/clock.c:1580-1600`）。截止时间是**当前核**的 `timer_call` 队列项；
核不再取定时器中断，队列就不再有截止时间到期，睡在上面的线程永久丢失。

## 3. 已知好镜像按字节复跑也不复现

`tmp/boot-qq1.img`（191,277,056 字节，`sheng-dpu-scanout=0`、`sheng-gpu-clients=2`、
addon `gpu-gf1`）在 11-05 21:35 出过桌面（`sw=67→72`、`PANEL nonblack=515/1000
sum=84338729`）。今天按字节复跑两轮：

- `qq1r1256471`：`ds=1`、`tf=892` 冻住、`sw=0 own=0`；
- `qq1r1301382`：没有任何 `SUPER` 行，只有 `EXITLOG xpcproxy`。

同一镜像两次都没出桌面。**桌面是启动竞态，不是显示配置项。**

## 边界

- 失聪核在 `cpus=4` 一轮里伴随整个控制台在 t=30 s 停止，并且 `sheng-autoreset=400`
  没有复位板子：硬停时自动复位不成立，板子需要手动上电。
- 逐核中断相位 `ph0` 目前分不清「不在中断处理程序里」与「符号未解析」；下一次读数
  会带 `ac=<线程 id>/<state>`（`cpu_datap(core)->cpu_active_thread`）把两者一起给出。
