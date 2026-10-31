# 用户提交为什么时有时无：读数与源码分析

2026-10-31 16:40。只读分析，未上板、未构建、未改 `tmp/parity-src`。
对比的三轮：`ibread3`（`uc 16/19`，有故障）、`clean1`（`uc 0/0`）、`cperr3`（`uc 0/0`）。

## 0. 结论

**`uc 0/0` 不是「render server 起来了但空闲」，而是 render server 的进程起来了、却从未打开 GPU 驱动。**
所以官方那句「空闲桌面每 250 ms 提交约 6 个命令缓冲区」在这个状态下不适用：它要求已经有一个
打开的客户端。故障时有时无的第一道门在**设备发现/插件加载**，不在提交频率。

证据链：

| 读数 | 值 | 说明 |
| --- | --- | --- |
| `[spawn]` census（cperr3） | `SpringBoard 2/0@28`、`backboardd 1/0@31` | render server 的进程起了 |
| `GPUROW23`（cperr3） | `uc 0/0 … rounds 0 0` | `native_adreno_publish_stats` 的 opened/closed 都是 0（`tmp/parity-src/NativeAdrenoPublish.cpp:975-978`） |
| `[gpu clients]` | 只有 `kernel_task` | 没有用户连接活着 |
| `[gpu windows]` | `mount fail stage 1 st e00002d5 slot 0 n 1` | 有一次 mount 失败；`stage 1` = 没拿到地址空间（`NativeAdrenoPublish.cpp:304-312`），`e00002d5` = `kIOReturnBusy` |
| `[gpu methods]` | `1:1 2:2 3:2 5:1 6:2/1 7:1 13:2 15:1` | 只有一轮内核客户端的调用，没有用户态的 Bulk |

## A. render server 根本没起来，还是起来了但没提交？

**起来了，没打开设备。**

- render server 的进程在 `[spawn]` 里（cperr3：`backboardd 1/0@31`）。
- 驱动侧 `g.opened` 为 0，`[gpu clients]` 只有 `kernel_task`；用户态的 `newUserClient` 一次都没成功。
- 因此不存在「backboardd 提交了 0 个 CB」这种情况——它连 `IOAcceleratorES` 的 user client 都没拿到。

`GPUROW23` 的 `uc` 与 `rounds` 来自 `native_gpu_publish[3..9]`（`package/GpuProbe.cpp:1166-1171`），
`uc` 由 `ucRound()` 的 step 计数（`:1212-1218`），`rounds` 是它的返回值。

## B. `--publish` 与 `--clients` 各自需要什么

我们的 addon 是 **publish 模式**：`strings tmp/gpu-parity4.gpuaddon` 里有
`addons/NativeAdrenoMetal.publish` 与 `addons/NativeAdrenoMetal.bundle/*`。

官方（`NEW/UniXNU/docs/zh/drivers/adreno740.md:36-45`）：

- `--publish`：backboardd 把原生设备交回原有发现流程，CARenderServer 通过它合成；
- `--clients`：每个列入的进程各得设备，**最多两个还获得 256 MiB 窗口和自己的 shader cache**。

插件侧（`NEW/UniXNU/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm`）：

- `publish::requested()` 在 bundle 旁边找 `NativeAdrenoMetal.publish`（`:50-59`）；
- 非 backboardd 进程在 `:300` 直接走 `clientDevice(port)`（不再读 `NativeAdrenoMetal.clients`，
  与 `reference.md` §31 的 29.4 一致）；
- backboardd 走 published 路径：要求 Turnip 库可读、且能打开一个可写的 shader-cache 目录，
  任一步失败返回 `nil`（`:305-317`，mark 0x68xx/0x6Axx 只写到驱动的进程文本行）。

所以产生用户提交的条件是三层：accelerator 注册成功 → bundle 被 backboardd 加载 →
published 设备创建成功（Turnip 库、shader cache）。缺任一层，`uc` 就是 0。

## C. 探针在 publish 之后等多久

探针不等：`native_gpu_probe()` 的顺序是
`publish()` → `selfTest()` →（`gMode>=12`）`metal()` → `heartbeat()`（`package/GpuProbe.cpp:1583-1586`）。
`heartbeat()` 是 `[[noreturn]]`（`:1412`），每 2 秒刷新一次 `GPUROW25`，所以 `uc` 反映的是整个
boot 的累计值。

`selfTest()` 跑两轮内核 `ucRound`，成功条件是两轮都返回 4（`:1297-1310`）；
**`metal()` 被 `selfTest()` 的返回值门控**。也就是说：内核自测失败时，accelerator 根本不会注册，
外部进程必然 `uc 0/0`。

`ibread3` 与 `cperr3` 的时间差不在等待：两者都在早期完成 publish，backboardd 都在 ~30 s 起来，
日志都覆盖到 178 s。差别在设备发现是否成功。

## D. `clientClose`/unmap 在 `uc 0/0` 的轮次里被调用过吗

没有。`open` 就没成功，`clientClose` 不会发生：
`NativeAdrenoUserClient::open/setScope/close` 在 `NativeAdrenoUserClient.cpp:120-165`，
`AGXDeviceUserClient::clientClose` 在 `NativeAdrenoPublish.cpp:394-413`（那里才 `++g.closed`）。
所以官方要求的「客户端退出、映射生命周期、fence 失败处理」这一条，在 `uc 0/0` 的轮次里
一项都没有被覆盖。

## E. 让一轮一定有用户提交：三条，按成本排序

1. **先补读数（不改行为）**：把 `GPUROW24`（METAL）加进持久日志的导出列表
   （`package/BootLog.c:1029` 的 `gpuRows[]` 改成 `{0, 23, 24, 25, 31}`）。
   代价：一次构建。判据：日志里 `GPUROW24` 是 `METAL ok` 还是 `METAL stopped at N: status`。
   这一步直接回答「accelerator 有没有注册」。
2. **解耦 `metal()` 与 `selfTest()`**：改成
   `const bool st = selfTest(); if (gMode >= 12) metal();`（`package/GpuProbe.cpp:1584`）。
   代价：一行，改变行为。判据：`GPUROW23` 的 `rounds` 仍是 `0 0` 而 `uc` 变成非零，说明
   瓶颈在内核自测而不是用户态。
3. **在 `metal()` 之后强制一次 `ucRound()`**（或每 30 秒一次），让「用户客户端路径」不再
   依赖 SpringBoard。代价：一次构建，且它测的是内核构造的提交，不是 Metal 的提交；判据：
   `GPUROW23` 的 `uc` 递增且 `submit > 0`。这条只能证明路径通，不能替代真实用户提交。

## 仍不一致的一处读数

`cperr3` 的 `GPUROW23` 写 `rounds 0 0`，但同一轮的 `[gpu methods]` 有一次
`Query/BoAlloc/BoMap/Submit/WaitFence/BoCpuMap/GetDeviceInfo` 的调用计数。
若 `selfTest()` 跑了 `ucRound()`，`p[4]`（步数）应大于 0；若没跑，方法计数不该动。
两者只能有一个对。下一轮把 `selfTest` 的 `on`、`svc`、两次返回值分别记录到独立字段，
不要再用同一个 `p[]` 行表示。

## 源码位置索引

- 探针顺序与门控：`NEW/UniXNU-alioth/package/GpuProbe.cpp:1583-1586`；`selfTest` `:1297-1310`；`metal` `:1316-1396`。
- mount 与 client 统计：`tmp/parity-src/NativeAdrenoPublish.cpp:304-335`、`:394-413`、`:975-978`。
- 用户客户端：`tmp/parity-src/NativeAdrenoUserClient.cpp:120-165`、`:312`（窗口检查）。
- 插件：`NEW/UniXNU/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm:50-59`、`:300`、`:305-317`。
- 厂商：`reference/k40-kernel/drivers/gpu/drm/msm/adreno/a6xx_gpu.c:68`（`a6xx_submit`）、
  `:492`（`a6xx_recover`）、`:519`（`a6xx_fault_handler`）、`:533`（`a6xx_cp_hw_err_irq`）、
  `:575`（`a6xx_fault_detect_irq`）；`drivers/gpu/drm/msm/msm_gpu.c:411`（`recover_worker`）；
  `drivers/gpu/drm/msm/msm_gem_submit.c:284`（iova 变化拒绝）、`:336-383`（reloc 校验）。
- 官方：`NEW/UniXNU/docs/zh/porting/gpu.md:55-57`、`:85`；`zh/drivers/adreno740.md:36-45`；
  `zh/drivers/adreno-metal.md:17-23`；`zh/porting/display.md:43`。
