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
| 冻结时点 | **uptime 100 s**（不是轮数），留 32 秒余量给轮次漂移，覆盖 **t=0..100 s** |
| slots 起始块 | 33 → **65** |
| slot_count | 509 → **507**（窗口 8192 块） |

块 33..64 原本是窗口的空尾，没有别的消费者搬家。

### 为什么按秒而不是按轮

`slots_written >= 60` 是 60 轮，但**轮次不是严格 2 秒**：每轮还有 UFS 写、每 30 轮的 GPT 检查、
诊断导出，实测 `uptime_s` 与 `rounds` 不成正比（u276：90 轮 / 178 s，平均 1.98 s；
但慢轮可以更久）。若到第 60 轮已经过了 135 秒，boot 最早的几条已被冲掉，
冻结下来的不是 `t=0` 开头——判据 1 会失败，而且看起来像「冻结没生效」，实际是拍晚了。

改用 `now >= LogFrozenAtS`（`now` 是 writer 已有的 uptime 变量）。**100 秒留 32 秒余量**，
足够吸收轮次漂移。`frozen_round` 仍记录实际是第几轮拍的。

---

## 2. 改动

`package/BootLog.c`，3 处：

1. `:189` 常量：加 `LogFrozenFirst=33`、`LogFrozenBlocks=32`、`LogFrozenAt=60`，
   `LogSlotFirst` 从 33 改成 `LogFrozenFirst + LogFrozenBlocks` = 65。
2. `:205` header：版本 4 → 5（`header_bytes` 152 → 160），追加 `frozen_first`、`frozen_round`
   （字节 148..155，0 = 尚未拍）。
3. `:1229` writer：在写完工作快照后，若 `!h.frozen_first && now >= LogFrozenAtS`，
   再 `copy_msgbuf` 一次并写入 `LogFrozenFirst`；**只有写成功才置 `h.frozen_first`**，
   所以读者不会看到一个指向失败写入的块号。

顺序安全：每轮开头 `memset(boot_log_buffer, 0, LogSlotBlocks*LogBlock)`，
冻结写发生在工作快照写之后、header 写之前，buffer 内容不影响本轮其它写入。

### 阅读器（本轮已改，不留到下一轮）

`read-sheng-log.py` 加 version 5 分支：

- `header_fields()` 只在 `header_bytes >= 156` 时读 `frozen_first`/`frozen_round`，
  旧镜像默认 `(0, 0)`。
- 输出多一个 `frozen.txt`：`frozen_first` 非零时写那 32 块；为零时写
  `# no frozen snapshot: header version N, frozen_first 0 (not taken)`。
  **写说明而不是空文件**，这样「没冻结」是可见的，不会被误当成「冻结了但没内容」。

`header.txt` 也多了 `frozen_first` / `frozen_round` 两行。`msgbuf.txt` 仍按 `msg_first`，
工作快照的读法不变。

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

**实现方式**：`read-sheng-log.py` 已输出 `frozen.txt`（见 §2）。判据 1 直接看它第一行的时间戳，
判据 2 看 `msgbuf.txt` 第一行，判据 3 比较两者。**三条都能从脚本产物直接读，无需手工。**

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
| 冻结快照在 t=100 s 覆盖 t=0..100 s | **推断**，按 132 s/圈算，**未实测** |
| 区域 33..64 无其他消费者 | **事实**，`grep LogFrozenFirst` 仅本处 |
| 单文件编译 | **事实**，rc=0 |
| 阅读器 v4 兼容 | **事实**，对 u276 的 v4 日志跑通，`frozen_first` 默认 0 |
| 阅读器 v5 分支 | **事实**，用改过 header 的副本模拟，`frozen_first=33` 时读出了内容 |
| 真机效果 | **未验证**，需要一轮长跑（≥240 s） |
