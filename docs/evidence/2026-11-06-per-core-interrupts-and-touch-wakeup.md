# 逐核中断读数，与触控线程的唤醒丢失

2026-11-06。设备仓库 `NEW/UniXNU-alioth` @ `ab53842422feadf058a0575b6e5a968623c6c052`，
基仓库 `NEW/UniXNU` @ `9118c39aff68890236e495ec7f37b635cedcec88`，
ArmCpu @ `4a03457ad39483dc398d647f5cfe01a06ed74463`。
**真机读数，USB 串口抓取，`fastboot boot`，不写槽。**

| 镜像 | sha256 前 16 位 | 与上一镜像的差别 |
| --- | --- | --- |
| `boot-s10.img` | — | 新增 `CPUT` 行（逐核定时器与 SGI 计数） |
| `boot-s11.img` | — | ArmCpu 合入上游 `eb0f053`+`93a5030`；`sheng-dpu-scanout=0` |
| `boot-s12.img` | `08afe64b392c8ea9` | `sheng-dpu-scanout` 回到 2；新增 `CPUTM` 行与 `SUPER` 的 `tc=` |
| `boot-s13.img` | `ba9e6aad470348cb` | 新增 `boot-ui-trace=1` |

原始抓取：`tmp/console-s10*.txt`、`tmp/console-s11*.txt`、`tmp/console-s12*.txt`、
`tmp/console-s13*.txt`；判据结果：`tmp/rounds-s10.txt`、`-s11`、`-s12`、`-s13`。

---

## 1. 逐核中断在启动之间不一致

同一镜像 `boot-s10.img`，两次采样的 `CPUT` 行（格式 `cN=<定时器中断>/<SGI>`）：

```
CPUT t=10s  irq=441187  ipi=29395  sent=29400  | c0=7504/30   c1=6/14      c2=5/3    c3=20/12   c4=396835/27367 c5=30397/1730  c6=5472/198
CPUT t=20s  irq=859726  ipi=35231  sent=35236  | c0=14150/30  c1=6/14      c2=5/3    c3=20/12   c4=802614/32605 c5=34017/2278  c6=5472/198
```

同一次启动稍后：

```
CPUT t=180s irq=5117179 ipi=1938982 sent=2055956 | c0=118171/36868 c1=20645/302939 c2=15284/184372 c3=13742/124904 c4=2571787/557871 c5=1423882/461949 c6=5472/198
CPUT t=190s irq=5146454 ipi=1955496 sent=2072716 | c0=124754/37336 c1=21351/307859 c2=15440/186304 c3=13796/126111 c4=2571787/557871 c5=1440688/466793 c6=5472/198
```

- c1/c2/c3 在前 20 秒几乎是零，到 180 秒才上万——它们起得很晚。
- **c6 在两次不同启动里都是 `5472/198`，从不增长。**
- c4 承担了 55 万次 SGI，全系统 190 秒共 190 万次 IPI（约 10 kHz）；`sent` 比 `ipi` 多 12 万。

`armcpu_smp_step[c]=7`（running）对这些核全部成立，`run=7`。

## 2. 触控线程停在一次成功的读之后

`console-s903542.txt`（`boot-s9.img`，桌面已接管）：

```
SUPER t=130s runs=130 calls=7 mon=7 sw=8/8/0 own=1 prep=8 data=2 tf=839 lp=0 tfrc=0 tfs=0 tlow=0 ...
```

- `tf=839` 不再增长，`lp=0`：停在 `kits/alioth-touch/FocalTouchProbe.cpp:595-596` 的 `IOSleep(6)`。
- `tfrc=0`：最后一次 `fts5652_frame` 返回成功。`tfs=0`：从未进入失败分支。
- `tlow=0`：面板 IRQ 引脚没有读到过低电平（该线本板未接）。

读失败会留下非零 `tfrc`，`publish()` 阻塞会停在 `lp=3`。两者都不是，所以是睡眠后的唤醒没有到达。
同一镜像下 `tf` 在别的启动里可以涨到 27000 以上，说明这不是驱动初始化失败。

## 3. 合入上游 ArmCpu 之后

`boot-s11.img` 第一次启动：

```
CPUT t=150s irq=4351852 ipi=2049512 sent=2088517 | c0=100381/8108 c1=7158/306199 c2=3325/156515 c3=2496/83462 c4=2214627/988785 c5=103686/2131 c6=1267672/330628
SUPER t=160s ... own=0 ... tf=23413 lp=0 tfrc=0 tfs=0 tlow=0
```

c6 由恒定的 `5472/198` 变为百万级。但同镜像第二次启动 `tf=888` 再次冻住，
所以这次合入改善了核的可用状态，没有消除唤醒丢失。

## 4. 显示路径的两项取值

`sheng-dpu-scanout=0`（上游文档的默认值）三次启动 `s110423221`、`s110430452`、`s110438083`：
全部 `ds=1`、`own=0`、`sw=0`、`PANEL nonblack=911`、无 `SURFACEL` 行。客户端一次都没有调用显示。
`sheng-dpu-scanout=2` 的 `boot-s12.img` 两次启动 `ds=4`、`own=0`、`sw=0`、`PANEL=911`，
同样无 `SURFACEL`；`boot-s13.img` 一次 `own=1 sw=9/9/0 lit=25`。

`sheng-fb-gpu=1` 未启用：`NativeGOPresenter.cpp:122` 是 `if(gpuMode)return kIOReturnUnsupported;`，
打开后不存在 CPU 拷贝，而 2026-11-04 mp1 的填屏读数表明 GPU 写不到 `0x9c000000`。

## 5. `boot-ui-trace=1` 没有产出栈

`console-s130513491.txt` 有六条 `STK none`。该次启动 `own=1 sw=9/9/0 lit=25`。
`boot_ui_trace_role_rows()` 返回空，说明 trace 从未 publish；工作线程是否走到第 44 次调用
（约 88 秒）尚未单独计过。

## 6. 边界

- 以上全部是 `fastboot boot` 的单次读数，同一镜像的启动结果在这块板上会变化，单次不能当复现率。
- `CPUTM` 行的 `ahead` 字段在全部核上打印为 `2147483647`（钳位上限），当前不能作判据；
  `short` 字段（截止时间已过）在所有采样里为 0。
- 未做持久日志回读（TWRP），因此 `[stacks]`、`[tick]` 一类只在槽里的行没有纳入本文件。
