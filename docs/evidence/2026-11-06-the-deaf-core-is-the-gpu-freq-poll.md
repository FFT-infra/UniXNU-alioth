> 本分支只带修复本身（`board.toml` 的 `sheng-gpu-dynfreq=0`）。文中用到的 `GICR` 探针字段
> `nm=` / `ms=` / `ts=` / `mon=` 在 `test-gemini` 分支上，实测那三轮也是在那里跑的。

# 失聪核的根因：没有时钟的 GMU 寄存器读

2026-11-06 傍晚。设备仓库 `NEW/UniXNU-alioth`。真机读数，USB 串口抓取，`fastboot boot`，
不写槽。三轮镜像，每轮只加一个探针或一个启动参数。

| 镜像 | 与上一镜像的差别 | 原始抓取 |
| --- | --- | --- |
| `boot-w11.img` | `GICR` 行加 `nm=`（`thread_get_thread_name`） | `tmp/console-w111414141.txt`、`tmp/console-w111421372.txt` |
| `boot-w12.img` | `GICR` 行头加 `ms=` / `ts=` / `mon=` | `tmp/console-w121440191.txt`、`tmp/console-w121447412.txt` |
| `boot-w13.img` | `sheng-gpu-dynfreq=0` | `tmp/console-w131452581.txt`、`tmp/console-w131500102.txt` |

## 1. 封死核的是我们自己的监视器线程

```
w111414141  GICR ... ac=6f/4 nm=boot_progress k=0 pr=92   （c6 失聪，有桌面）
w111421372  GICR ... ac=6f/4 nm=boot_progress k=0 pr=92   （c5 失聪，无桌面）
```

`ac=` 是失聪核的 `cpu_active_thread`，`state=4` 是 `TH_RUN`（不是 `TH_RUN|TH_IDLE`）。
名字来自 `thread_get_thread_name`，内核线程也覆盖。**不是 `kernel-high`**：`pr=92` 与
`BASEPRI_PREEMPT` 相等曾让人以为是定时器回调线程，但 `kernel_thread_create` 的默认优先级
也是 92，这条推理不成立，只有名字能定。

`boot_progress` 是页面监视器线程（`BootProgress.c:1814` 的 `monitor()`）。它两轮都被封，
**其中一轮照样出了桌面**，所以它被封是常态，不是桌面的判据。

## 2. 步骤计数器指向一个函数

`monitor()` 每一步之间写 `boot_progress_monitor_step`（1..11），`boot_log_tick` 写自己的
`boot_log_tick_step`。两者由独立的 `corehealth` 线程打印，所以监视器自己封死时读数照样出得来。

```
w121440191  GICR ... ms=4 ts=4 mon=7
w121447412  GICR ... ms=4 ts=4 mon=7
```

`mon=7`：监视器循环只转了七圈（约七秒）就再没动过。
`ms=4`：它停在 `monitor()` 的第 4 步，即 `boot_log_tick()` 里面。
`ts=4`：`boot_log_tick` 停在 4。

对照 `BootLog.c`：

```c
boot_log_tick_step = 3;
if (&alioth_dpu_probe_tick) alioth_dpu_probe_tick(cur_now);   // 这一步返回了
boot_log_tick_step = 4;                                        // ts 停在这里
if (&native_gpu_freq_tick) native_gpu_freq_tick(cur_now);      // 卡在这里面
boot_log_tick_step = 5;                                        // 永远没写下去
```

DPU 探针被排除（它返回了）。停的是 `native_gpu_freq_tick()`（`GpuProbe.cpp:634`）：它向 GMU
发 HFI 电压投票，并读 `kGmuBusyHi/Lo` 忙计数器。这个探针在 `board.toml` 里从未设置过
`sheng-gpu-dynfreq`，代码默认 `on=1`，**所以每次启动都在跑**。

## 3. 关掉它，失聪核消失

`boot-w13.img` 只改了这一个启动参数（payload 重打包，内核不变）。

```
w131452581  t=190s  sw=124/124/0 own=1 prep=4833 tf=27572   deaf=00 wd=0/00 mon=191  GICR 行数 0
w131500102  t=110s  sw=149/149/0 own=1 prep=149  tf=15977   deaf=00 wd=0/00 mon=111  GICR 行数 0
```

两轮都是：八核全活、一次正式判定都没有、监视器每秒一圈（之前七圈就停）、桌面在 t≈80s 起来、
触控帧计数持续增长。**用户在同一块板上确认：桌面可见、触控可用。**

## 4. 机制：一颗核停摆，受害线程决定症状

没有时钟的 GMU 窗口上的寄存器读不返回，执行它的核停在总线上，中断再也进不来
（`ISPENDR0` pending、使能、`ISACTIVER0=0`、`WAKER=0`）。本板 `cpu_idle_latency` 为 0，
`IOSleep` 的截止时间挂在该核的定时器队列上，于是睡在那颗核上的线程永久丢失。

**丢的是谁，决定看到什么：**

| 丢失的线程 | 症状 |
| --- | --- |
| `mediaserverd` 启动链上的 | 渲染服务不起、`sw=0`、屏幕停在固件图（黑屏） |
| 触控循环线程 | 桌面在、触控无反应 |
| `boot_progress` 自己 | 页面与持久日志停在第七圈 |

2026-11-06 当天 39 轮里只有 18 轮 `own=1`，其中持续合成（`sw≥48`）的只有 3 轮；
这个比例就是"受害线程恰好不重要"的概率。

## 边界

- 重新打开 `sheng-gpu-dynfreq` 的前提不是"GX 轨已上电"，而是**读的那一刻 GMU 时钟已开**。
  现在的 `gColdOk && gx_on` 判断不足以证明这一点。
- 本轮只证明这条读路径会挂住核；GMU 寄存器读在其他路径（`gpuBusyTicks` 之外）是否同样不安全，
  没有单独测量。
- `PANEL nonblack` 在 `ds=4`（直通扫描）下读的是固件帧缓冲，恒为 911，不能用于判断桌面是否出现。
  判据是 `sw` 的接受数与 `own`。
