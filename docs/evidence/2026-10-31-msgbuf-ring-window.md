# IOLog 已经进持久日志；缺的是保留窗口，不是通路

2026-10-31。设备仓库 `NEW/UniXNU-alioth-usb` @ 分支 `usb-work`。
**只读分析 + 单文件编译验证。未跑 `build kernel`，未上板。**

---

## 1. 【做了什么】

1. 逐行读 `_IOLogv` → `os_log_with_args` → `_os_log_to_msgbuf_internal` →
   `vprintf_log_locked` → `log_putc_locked` 的调用链。
2. 读 `BootLog.c` 的持久日志写入路径与区域常量。
3. 对三份真实运行产物做文本判定（u276 / u278 / u259）。
4. 把 `BootLog.c` 的 msgbuf 区从固定块改成 4 块环形，单文件编译验证。

---

## 2. 【关键证据】

### 2.1 前提被推翻：IOLog 确实进 msgbuf

交办前提是「`_IOLogv` 走 os_log，不是 kprintf，所以不进 msgbuf、不进持久日志」。
**后半句不成立。**

```
IOLog(fmt, ...)                                   iokit/Kernel/IOLib.cpp:1558
  _IOLogv(fmt, ap, caller)                        iokit/Kernel/IOLib.cpp:1571
    os_log_with_args(OS_LOG_DEFAULT, ..., ap)     iokit/Kernel/IOLib.cpp:1581   ← 无条件
os_log_with_args                                 libkern/os/log.c:884
  _os_log_with_args_internal                      libkern/os/log.c:884
    if (oslog != &_os_log_replay)                 libkern/os/log.c:788
      _os_log_to_msgbuf_internal(fmt, args)       libkern/os/log.c:789   ← 进 msgbuf
        printf_log_locked("[%5lu.%06u]: ")        libkern/os/log.c:708   ← 时间戳
        vprintf_log_locked(...)                   libkern/os/log.c:714
          pca.flags = TOLOGLOCKED                 bsd/kern/subr_prf.c:429
          putchar -> log_putc_locked(msgbufp)     bsd/kern/subr_prf.c:391
持久日志
  copy_msgbuf(boot_log_buffer)                    BootLog.c:1195
  native_ufs_log_write(LogMsgFirst, ...)          BootLog.c:1197
```

三条旁证：

| 检查 | 结果 |
| --- | --- |
| `OS_LOG_DEFAULT` 是不是 `OS_LOG_DISABLED` | **否**，展开为 `_os_log_default`（`libkern/os/log.h:89,97`） |
| `:789` 的调用是否在 `if (safe && logging)` 内 | **否**，那个 if 只管第二段（进 os_log 存储） |
| `disable_iolog_serial_output` 默认值 | **false**（`pexpert/arm/pe_kprintf.c:35`），只关第二段（→串口） |

### 2.2 运行时铁证

`build/sheng-logs/u259-000000044334be6e/msgbuf.txt`：

```
[  986.432796]: void CoreAnalyticsHub::handleNagTimerExpiry(IOTimerEventSource *)::751:messageClients of 1 available events
[  989.265613]: virtual bool CoreAnalyticsUserClient::start(IOService *)::120:success
[  989.265616]: IOReturn CoreAnalyticsHub::setClientGated(CoreAnalyticsUserClient *)::375:adding userClient
```

`__PRETTY_FUNCTION__::__LINE__:msg` 是 kext 的 IOLog 惯用形。带 `[timestamp]:` 前缀，
而那个前缀**只有** `libkern/os/log.c:708` 会加。格式串在 kernelcache `0xa7904d` 处
（`%s::%d:messageClients of %d available events`），不在我们源码树里，所以必然来自 kext。

### 2.3 真缺口：保留窗口

| 运行 | rounds | msgbuf 最老时间戳 |
| --- | --- | --- |
| u278 | 3 | `61.828`（boot 期完整） |
| u276 | 90 | `119.894`（已绕过 boot 期） |
| u259 | 88 | `986.433`（已绕过 boot 期） |

- msgbuf 大小 `CONFIG_MSG_BSIZE_DEV = 131072`（`config/MASTER:286`）
- 实测速率约 **996 B/s** → 128 KiB ≈ **132 秒**
- `LogMsgFirst` 是**固定块**，每 2 秒一轮覆盖写同一块

**所以长跑会把 boot 期的 IOLog 冲掉**——而 kext probe 失败正是发生在 boot 早期。

---

## 3. 改动

`package/BootLog.c`，2 处：

1. `:189` 常量表：`LogMsgFirst=1, LogMsgBlocks=32` → 增加 `LogMsgRing=4`，
   `LogSlotFirst = LogMsgFirst + LogMsgBlocks * LogMsgRing`（33 → 129）。
2. `:1219` 写入：目标块从固定 `LogMsgFirst` 改为
   `LogMsgFirst + (h.slots_written % LogMsgRing) * LogMsgBlocks`，
   并把 `h.msg_first` 指向本轮块。

**阅读器不用改**：`read-sheng-log.py:156` 按 `h.msg_first` 定位。

### 区域账

| 项 | 改前 | 改后 |
| --- | --- | --- |
| msg 区 | 块 1..32（128 KiB） | 块 1..128（512 KiB） |
| slot 起始块 | 33 | 129 |
| slot_count（窗口 8192 块） | 509 | 503 |
| 保留窗口 | ~132 s | ~8.8 min |

块 33..128 原本是窗口的空尾，没有别的消费者，所以没有区域搬家。
`h.slots_written` 是 `uint32_t`（`BootLog.c:196`），`% 4` 不会回绕或溢出。

### 越界判定

| 文件 | 属主 | 越界 |
| --- | --- | --- |
| `NEW/UniXNU-alioth-usb/package/BootLog.c` | 设备仓库（本会话 worktree） | **否** |
| `NEW/UniXNU/iokit/Kernel/IOLib.cpp` | 基仓库通用 | 是（**未改**） |
| `NEW/UniXNU/libkern/os/log.c` | 基仓库通用 | 是（**未改**） |

**跨板影响：零。** `BootLog.c` 是 alioth 专属；sheng / umi 用自己的 package。

---

## 4. 验收（可判定）

**判据**：一次长跑（`sheng-autoreset` ≥ 240 s）后，
`read-sheng-log.py` 产出的 `msgbuf.txt` 的**第一行时间戳**回到 boot 期。

```
改前：head -1 msgbuf.txt  ->  [  119.893523]: ...
改后：head -1 msgbuf.txt  ->  [    0.xxx]: ...   或   [   61.828015]: ...
```

**这是纯文本判定，不需要新代码路径。** 具体文件：该轮 `build/sheng-logs/<tag>-<boot_id>/msgbuf.txt`。

要等到 A-5 才有「只出现在 IOLog 的 USB 文本」可查，所以现在用「最老时间戳回到 t≈0」这条。
A-5 之后的加强判据：`msgbuf.txt` 里出现
`AppleEmbeddedUSBArbitrator` / `AppleNubSynopsysOTGDevice` 的启动行——
那些是 `AppleEmbeddedUSB.kext` 的 `IOLog`，`AppleSynopsysOTGDevice.cpp` 的
`start` 会打 `AppleSynopsysOTGDevice::start : registers at %p, %p physical`
（kernelcache 字符串表 `0x556818c`）。

### 编译验证

按产线 recipe（`osfmk/DEVELOPMENT/startup.o.json` 的 469 参数）单文件编译：

```
rc = 0
-rw-r--r--  392360  BootLog.o
499 warnings（全部是 XNU 头的 -Wunsafe-buffer-usage，与本次改动无关）
```

**未跑 `build kernel`**（按协调规则，GPU 会话正在换源）。

---

## 5. 边界

| 项 | 状态 |
| --- | --- |
| IOLog → msgbuf | **事实**，逐行可查 + 运行时产物 |
| msgbuf → 持久日志 | **事实**，`BootLog.c:1195,1197` |
| 128 KiB / ~132 s | **事实**，配置值 + 实测速率 |
| 环形后 ~8.8 min | **推断**（4 × 132 s），**未实测** |
| 块 33..128 无其他消费者 | **事实**，`grep LogSlotFirst` 只有一处定义与两处使用 |
| 单文件编译 | **事实**，rc=0 |
| 真机效果 | **未验证**，需要一轮长跑 |
