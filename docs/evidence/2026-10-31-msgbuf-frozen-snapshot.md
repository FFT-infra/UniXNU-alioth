# 冻结一份 boot 期 msgbuf 快照：IOLog 在长跑后仍可读

2026-10-31。设备仓库 `NEW/UniXNU-alioth-usb` @ 分支 `usb-work`。
**单文件编译验证。未跑 `build kernel`，未上板。**

---

## 0. 先更正我上一版的错误

上一版（已 revert，`250969d`）把 msgbuf 区做成 4 块轮转，注释写
「retained window from ~132 s to ~8.8 min」。**那是错的。**

`copy_msgbuf`（`BootLog.c:1117`）每次拷贝的是**内核 msgbuf 的最新 131072 字节**。
而 `CONFIG_MSG_BSIZE_DEV` 也是 131072（`config/MASTER:286`），所以：

```
keep  = min(size, cap) = min(131072, 131072) = 131072 = size
start = (bufx + size - cap) % size = bufx          <- 环的最老字节
```

**每次拷的都是整环**。4 个块里是 4 个**重叠**的窗口，彼此只差 2 秒（一轮）。
保留窗口 ≈ 132 + 3×2 = **138 秒**，不是 8.8 分钟。

根因不在「写到哪个块」，在**内核 msgbuf 本身只有 132 秒**。磁盘上再怎么轮转，
也变不出内核里已经不存在的字节。

---

## 1. 正确解法：冻结一份早期快照

要保的是 A-5 那条：失败的 kext probe 在 boot 早期打的 IOLog，长跑后被冲掉。

**在 msgbuf 还没绕过 boot 段之前，把当时那份快照写进一个永不复用的块。**

| 项 | 值 |
| --- | --- |
| 工作快照 | 块 1..32（128 KiB），每轮覆盖，行为不变 |
| **冻结快照** | **块 33..64（128 KiB），`slots_written >= 60` 时写一次，此后永不触碰** |
| 冻结时点 | 第 60 轮 ≈ **t=120 s**，仍在第一圈内，所以那份快照覆盖 **t=0..120 s** |
| slots 起始块 | 33 → **65** |
| slot_count | 509 → **507**（窗口 8192 块） |

块 33..64 原本是窗口的空尾，没有别的消费者搬家。

### 为什么第 60 轮是对的

msgbuf 一圈约 132 秒（实测 996 B/s）。第 60 轮在 t≈120 s，**还在第一圈内**，
所以那一次 `copy_msgbuf` 拿到的是 `t=0..120 s` 的完整内容——boot 期 kext probe
的 IOLog 全在里面。晚于此写，boot 段已经开始被覆盖；早于此写，日志还不完整。

---

## 2. 改动

`package/BootLog.c`，3 处：

1. `:189` 常量：加 `LogFrozenFirst=33`、`LogFrozenBlocks=32`、`LogFrozenAt=60`，
   `LogSlotFirst` 从 33 改成 `LogFrozenFirst + LogFrozenBlocks` = 65。
2. `:205` header：版本 4 → 5，追加 `frozen_first`、`frozen_round`（0 = 尚未拍）。
3. `:1229` writer：在写完工作快照后，若 `!h.frozen_first && slots_written >= LogFrozenAt`，
   再 `copy_msgbuf` 一次并写入 `LogFrozenFirst`；**只有写成功才置 `h.frozen_first`**，
   所以读者不会看到一个指向失败写入的块号。

顺序安全：每轮开头 `memset(boot_log_buffer, 0, LogSlotBlocks*LogBlock)`，
冻结写发生在工作快照写之后、header 写之前，buffer 内容不影响本轮其它写入。

### 阅读器

`msg_first` 仍指工作快照，**现有阅读逻辑不变**（`read-sheng-log.py:156`）。
冻结区是**额外**的一份，块号在 header 尾部的新字段 `frozen_first`。
旧阅读器忽略未知尾部字段，不会报错。

### 越界

| 文件 | 属主 | 越界 |
| --- | --- | --- |
| `NEW/UniXNU-alioth-usb/package/BootLog.c` | 设备仓库（本会话 worktree） | **否** |
| `NEW/UniXNU/iokit/Kernel/IOLib.cpp` | 基仓库通用 | 是（**未改**） |
| `NEW/UniXNU/libkern/os/log.c` | 基仓库通用 | 是（**未改**） |

**跨板影响：零。** `BootLog.c` 是 alioth 专属。

---

## 3. 验收（可判定，按总控要求改）

**条件**：一次长跑，`sheng-autoreset` ≥ 240 s（≥ 120 轮）。

**三条判据，必须同时成立**：

1. **冻结区第一个时间戳在 boot 期**
   ```
   读块 h.frozen_first 起的 128 KiB，取第一个 [t] 前缀
   应为 [0.xxx] 或 [6x.xxx]
   ```
2. **工作快照的第一个时间戳在 120 秒之后**
   ```
   head -1 msgbuf.txt（msg_first 指向的工作快照）
   应为 [1xx.xxx] 或更大
   ```
3. **两者不同**——这才证明冻结生效，而不是两份都在看同一个窗口。

第 3 条是关键：如果两个块的时间戳相同，说明冻结没写进去（或写了但被覆盖）。

**实现方式**：`read-sheng-log.py` 目前只读 `msg_first`。要验第 1 条，用
`frozen_first` 手工定位，或在脚本里加一个 `frozen.txt` 输出。**这是下一轮的事**，
本报告只定义判据。

### A-5 之后的加强判据

`msgbuf.txt` 出现 `AppleSynopsysOTGDevice::start : registers at %p, %p physical`
（kernelcache 字符串 `0x556818c`）——那是 Apple DWC2 驱动 `start` 的 IOLog，
**恰好就是 A-3 跑起来会打的那条**。所以冻结改动与 A-3 的验证用同一条判据。

---

## 4. 编译验证

产线 recipe（`osfmk/DEVELOPMENT/startup.o.json`，469 参数）单文件编译：

```
rc = 0
-rw-r--r--  393496  BootLog.o
0 errors，499 warnings（全是 XNU 头的 -Wunsafe-buffer-usage）
```

**未跑 `build kernel`**（GPU 会话正在换源）。

---

## 5. 边界

| 项 | 状态 |
| --- | --- |
| IOLog → msgbuf | **事实**，逐行 + 运行时产物（u259 有带时间戳的 kext IOLog） |
| msgbuf 只有 131072 B / 约 132 s | **事实**，配置值 + 实测速率 |
| 轮转多个块不能延长窗口 | **事实**，算术（keep == size，每次拷整环） |
| 冻结快照在 t≈120 s 覆盖 t=0..120 s | **推断**，按 2 s/轮与 132 s/圈算，**未实测** |
| 区域 33..64 无其他消费者 | **事实**，`grep LogFrozenFirst` 仅本处 |
| 单文件编译 | **事实**，rc=0 |
| 真机效果 | **未验证**，需要一轮长跑 |
