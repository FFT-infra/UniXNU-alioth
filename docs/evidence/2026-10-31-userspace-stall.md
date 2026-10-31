# 「进用户态后停住」：三次记录不同形，共同点不是一条消息

2026-10-31。**离线分析，未上板、未构建。** 设备仓库 `NEW/UniXNU-alioth-usb` @ `e276c20`。

计划 `PLAN-EXEC-2026-10-31.md` §G2 的提法是「三次同形，找三条里最后出现的同一条消息」。
按这个提法做了一遍，**结论是三次并不同形，也不存在一条三处共有的消息**。下面先给实测，
再给能区分三种假设的判据。这个否定本身比强行找一个共同点有用，因为它把「一条线」拆成了两条。

---

## 1. 三次各自的最后一条消息（逐字）

| 轮次 | 文件 | 最后一条非空行（逐字） |
| --- | --- | --- |
| `a6xx2` | `tmp/console-a6xx2-0518.log` | `TOUCHSTAT t=175s frames=25197 valid=25197 contacts=0 irq=1 low=749 hid=1024 xy=27502,3363 raw=000043ce018d hstage=8 hload=0 hml=0 hmeta=0 hcls=208 hevdrv=264 hres=0 edge=1,16,0 loop=2 fail=0 tsplit=0` |
| `wifi12` | `tmp/console-wifi12r1.log` | `TOUCH pins cfg=00000084,00000084,00000084 want=00000084,00000084,00000084` |
| `wifi13r1` | `tmp/console-wifi13r1.log` | `TOUCH app=0000 rc=0/0/0` |
| `bt4` | `build/sheng-logs/bt4-000000004df5b202/latest.txt` | `PK ba1a384:397 ba18f5c/ba19434/b9a7d24:393 ba18f5c/ba19520/b9c22c4:4` |

**注意两处与计划文本不一致，先记下来：**

- 计划写「`wifi12`（`step 12: done` 之后还有 40 行 `TOUCH`）」。实测 `step 12: done` 在
  `wifi12r1` 的第 311 行，文件共 351 行，**后面确实是 40 行**。但这 40 行里没有一行是
  `TOUCHSTAT`；它们是 `PCIE 13/14` 的收尾、两段 `rsc=/cmd-db/TCS0` 的寄存器转储、5 行
  `TOUCH start/ready/app/tlmm/pins`。`TOUCHSTAT` 在 `wifi12r1` 里**一条都没有**。
- 计划写「三次」。可 `btn5` **没有日志**（`build/sheng-logs/btn5-*` 不存在，
  `progress/2026-10-31.md:26` 已记）。能拿到的第三份是 `bt4`，它不是控制台捕获，
  是持久日志的一个 slot。用 `bt4` 替代 `btn5` 是唯一可行的选择，但两者证据类型不同，见 §3。

`a6xx2` 的原始 `tmp/console-a6xx2.log` 在本轮 07:49 被另一个会话的 `round-a6xx.sh`
**重写**（同一路径，`console-capture.py` 以 `wb` 打开）。05:18 的副本已被保存为
`tmp/console-a6xx2-0518.log`（49,480 字节，md5 `5aac830008568c4008cc466328028221`），
本表引用的是这一份。**读数在 07:49 之后从 `tmp/console-a6xx2.log` 取会得到另一次启动的内容。**

## 2. 有没有「三条里最后出现的同一条消息」

做的是逐字比较，再退一步做归一化比较（数字与十六进制替换成占位符），再退一步做关键词比较。

| 比较 | 结果 |
| --- | --- |
| 三个文件尾 40 行逐字交集 | **0 条** |
| 三个文件全文逐字交集 | **0 条** |
| 归一化（数字→`<n>`，长十六进制→`<hex>`）后全文三向交集 | **0 条** |
| `a6xx2` 与 `wifi13r1` 两两 | 全文 43 条相同，最后一条是 `TOUCH app=0000 rc=0/0/0` |
| `wifi13r1` 与 `wifi12r1` 两两 | 全文 68 条相同，最后一条是 `TOUCH app=0000 rc=0/0/0` |
| `a6xx2` 与 `bt4` | **0 条**（一份是控制台捕获，一份是 slot 文本，无共同行） |

**所以「三条共有的最后一条消息」不存在。** 两两比较里最靠后的共有行是 `TOUCH app=0000
rc=0/0/0`，但它在 `a6xx2` 是第 101 行（共 641 非空行），也就是**开机早期**，不是停住的位置。
把「共有的最后一条」当成「停住的位置」会得出「停在 TOUCH app」——这与 `a6xx2` 之后又跑了
74 秒 `TOUCHSTAT` 的事实矛盾。

## 3. 三份证据不是同一类东西（这是关键）

| 轮次 | 文件类型 | 覆盖到什么时刻 | 内核在跑吗 |
| --- | --- | --- | --- |
| `a6xx2` | `console-capture.py` 的串口捕获 | 最后一条 `t=175s`；捕获器 420 s 后自停 | 是：持久日志 `uptime 176 s`、`state 2`、`rounds 89` |
| `wifi12` | 同上 | 最后一条无时间戳；捕获器起于 05:41:33，**文件 mtime 05:41:46**，之后 236 s 无写入 | **无持久日志可查**（`build/sheng-logs/wifi12-*` 不存在） |
| `wifi13r1` | 同上 | 捕获器起于 05:58:28，**文件 mtime 05:58:40**，之后 229 s 无写入 | 同上，无持久日志 |
| `bt4` | 持久日志 `latest.txt` 的一个 slot | `#slot 89 t=178s`，`uptime_s 178`，`state 2` | 是（header 由写线程自己写的） |

`wifi12`/`wifi13` 的「停住」是**控制台在 t≈13 s 之后不再有字节**，而这一轮又没有读回持久日志，
所以**这两轮到底有没有停住，现有证据回答不了**。它们的 `round-*.out` 记的是
`DID NOT SELF-RETURN within 240s`，那是一个超时读数，不是内核状态。

## 4. 能区分三种假设的判据

计划要求一条能把「停在某条 kernel 消息 / 停在 launchd 某一项 / 根本没停只是 autoreset 没触发」
分开的判据。**下面这条能把三者全部分开，而且它只看一份持久日志，不需要控制台。**

### 判据：读回同一轮的持久日志，看 `[page]` 的 `time=` 与写线程的 `state`

| 观察 | 结论 |
| --- | --- |
| 有持久日志，`state = 2` 且 `uptime_s` 停在某个值不再增长 | **内核在跑、写线程在跑**：板子没有死，是某一项用户态工作没有完成，或 autoreset 没有触发（假设 2 或 3） |
| 有持久日志，`state = 3` | 写线程因连续 3 次写失败退出（`BootLog.c:1280` `errors >= LogMaxErrors`）→ **停住发生在 UFS 写路径上**，与 launchd 无关 |
| 有持久日志，`state = 4` | GPT 在读回时变了（`BootLog.c:1281`）→ 有人写了分区表 |
| 有持久日志，但 `uptime_s` 与 `rounds` 都很小（如 4 s / 3 轮） | 写线程启动后很快停 → 看 `[reset]` 段 |
| **完全没有持久日志** | 写线程从未写出 header：不是「用户态停住」，是更早的路径（或镜像里 `sheng-log` 没开） |

`a6xx2` 与 `bt4` 都落在第一行：`state 2`、`uptime 176/178`、`rounds 89/90`。
**它们不是「内核停住」，是内核在跑而某一项没有完成。**

### 再分「停在 kernel 消息」与「停在 launchd 某一项」

`[spawn]` 段给出每个进程的退出计数，格式 `名字 退出次数/失败次数@启动秒`。
`bt4` 的最后几项：

```
AppPredictionIntentsHel 2/0@75  IMDMessageServicesAgent 2/1@119
```

`@119` 是**启动秒**，它大于 `[page]` 的 `time=184s` 之前的绝大多数行，说明 launchd 在 t=119 s
还在起进程。如果停住发生在 launchd 某一项，那么该项之后的进程**没有 `@` 记录**，而该项本身
会有 `2/0@N` 但后面没有任何更新。反过来，如果 `[spawn]` 的最后一项 `@N` 接近 `uptime_s`，
说明 launchd 一直工作到最后，停住不在这里。

**这是可判定的一条**：`bt4` 最后一项 `IMDMessageServicesAgent 2/1@119`，而 `uptime 178`，
差 59 s。**launchd 在最后 59 秒没有记录任何新的启动**——这与「停在 launchd 某一项」一致，
但不能区分「那一项卡住」与「launchd 自己停了」。

### 三假设的最终判据（一条命令）

```bash
grep -E '^time=|^stage: target pid=' <log>/latest.txt | tail -1
```

- 有 `stage: target pid=N <name> exec process signature accepted` 且该行**晚于** `[spawn]`
  的最后一项 → launchd 仍在推进，停住不在 launchd；
- 没有该行、`[spawn]` 最后一项的 `@N` 远小于 `uptime_s` → **launchd 在 t=N 之后不再启动任何进程**；
- 该行存在但 `pid` 与 `[spawn]` 里同名项的 pid 不同 → 同一项被重启过，说明它先失败过一次。

## 5. autoreset 为什么没有触发（可判定的部分）

`BootLog.c:1333-1355` 的 `boot_log_tick` 负责 autoreset，`board.toml` 的
`sheng-autoreset=180`。它的判据是 `elapsed = now - tick_first >= tick_autoreset`，且
`now - tick_seen_at <= ResetSyncGrace`（10 s，`BootLog.c:258`）。

**实测：所有 `state 2` 的持久日志都停在 `uptime 178 / rounds 90`。** 90 轮 × 2 s
（`LogPeriodMs = 2000`，`BootLog.c:219`）= 180 s，与 `sheng-autoreset=180` 是同一个数。

| 日志 | uptime | rounds | rounds×2 | 结局 |
| --- | --- | --- | --- | --- |
| `wifi1` | 178 | 90 | 180 | 自回 195 s |
| `wifi3` | 178 | 90 | 180 | 自回 195 s |
| `btn4` | 178 | 90 | 180 | — |
| `btn6` | 178 | 90 | 180 | — |
| `bt4` | 178 | 90 | 180 | — |
| `a6xx2` | 176 | 89 | 178 | 225 s 自回（`progress/2026-10-31.md:244`） |
| `u239` | 600 | 300 | 600 | — |

**这是本轮最有用的一条读数**：写线程恰好停在 autoreset 的截止点上。两种解释都成立，
而且可以分开：

- **A（写线程被 autoreset 的重启打断）**：180 s 到点时 `boot_reset_bootloader` 开始执行，
  板子重启，写线程不再有下一轮。此时 `[reset]` 段应有 `boot_log_reset_result` 的文本
  （成功则机器重启，失败则留下 `returned <rc>`）。
- **B（写线程自己在 180 s 停下）**：`state` 会变成 3 或 4，或 `[reset]` 段的
  `boot_log_reset_result` 为空（`BootLog.c:1074` 只在非空时输出）。

**实测：`a6xx2`、`bt4`、`btn4`、`btn6` 四份 `[reset]` 段里 `autoreset`/`returned` 的行数都是 0。**
即 `boot_log_reset_result` 为空。按 A 的解释，成功的重启不会留下文本，所以「空」与 A 一致；
但按 B 的解释，`state` 仍是 2，与 B 的「3 或 4」不符。**倾向 A，但这条不能只靠一个空字段定案。**

`[reset]` 段实测（`a6xx2`，逐字）：

```
psci v10001 reset=0 reset2_32=0 reset2_64=0 | imem 146bf65c = 6ab830e2 known=0
mark: 6ab830e2 -> 77665500 rc=0 | pon: subtype 04 shift 1 spare 00 -> 04 want 04 rc=0 ok pon_rc=0
watchdog: not armed; kona has no Gunyah and the APSS watchdog protocol is not implemented (deadline 240s)
```

**`watchdog: not armed` 是硬事实**：Hypervisor watchdog 没有武装，`deadline 240s` 只是一个
名义值。所以「板子 240 s 后被 watchdog 咬回来」这条路径不存在，自回只能来自
`boot_reset_bootloader` 或人工断电。

## 6. 三次记录里真正不同的那一件事：心跳

`BLPHASE` 由 `boot_log_tick` 每秒打印（`BootLog.c:1325-1330`，相位变化或每 10 s 一次）。
它是**唯一一条由 autoreset 的同一个线程发出的控制台行**。

| 轮次 | `BLPHASE` 条数 | 最后一条 | 自回？ |
| --- | --- | --- | --- |
| `wifi1` | 19 | `t=173s` | 195 s |
| `wifi3r3` | 19 | `t=173s` | 195 s |
| `wifi10r1` | 1 | `t=1s` | 36 s |
| `wifi11r1` | 1 | `t=1s` | 48 s |
| `wifi12r1` | 1 | `t=1s` | **未自回** |
| `wifi13r1` | 1 | `t=1s` | **未自回** |
| `a6xx2` | 19 | `t=173s` | 225 s |

**`wifi10/11` 也只有一条 `BLPHASE` 却 36/48 s 就自回**，所以「心跳只有一条」不等于停住。
这两轮 `pcie=12`/`pcie=13`，PCI 探测只走到 step 11/12 就结束，**开机流程比 `wifi12/13` 短**，
所以它们在 `BLPHASE t=1s` 之后很快走完并重启，第二条心跳还没来得及打。

**这修正了 `progress/2026-10-31.md:244` 的一句**：那里写 `wifi12` 是「用户态某处停住」。
实测 `wifi12r1` 的 PCIE 走完了 14 步（`step 12: done` 在第 311 行、`steps done` 在第 312 行），
`wifi13r1` 走完了 14 步，两者都**没有** `TOUCHSTAT`。而 `wifi1`/`wifi3r3` 走完 PCIE 后有 37 条
`TOUCHSTAT`。所以 `wifi12/13` 与 `wifi1/3r3` 的差别**不在用户态**，在 PCIE 之后的
触摸采样循环没有产出。

## 7. 下一轮的单变量方案

计划要「单变量」。按上面 §5 的 A/B 分叉，**第一个变量不是代码，是读数方式**：

**G2-a（不上板，零成本）**：`wifi12`/`wifi13` 这两轮**没有持久日志**，所以它们现在不可判定。
下一轮无论跑什么，`round-*.sh` 都必须在板子回来后读回持久日志（`PLAN-EXEC` §D1 第 8 步）。
**没有这一步的轮次不产生证据，只产生一个超时读数。**

**G2-b（一次上板，单变量 = 加一条 reset 结果行）**：在 `boot_log_reset_result` 为空时也写一行
`autoreset: not attempted (t=<now>, seen_at=<tick_seen_at>)`，让 §5 的 A/B 在日志里直接可读，
不必靠「空字段」推断。**这是改 `BootLog.c` 一行，属于设备仓库，不越界。**

**G2-c（一次上板，单变量 = 心跳间隔）**：`BLPHASE` 现在「相位变化或每 10 s」打印一次
（`BootLog.c:1326`），而 `wifi10/11` 证明**短流程根本来不及打第二条**。把心跳改成无条件每秒
一条，`wifi10` 这种 36 s 自回的轮次就会有 36 条，`wifi12/13` 若真停住就会有 1 条——
**「1 条 vs N 条」才成为可判定的读数**。这条同时给出 §6 那张表缺的那一列。

**顺序**：先 G2-a（它让后面每一轮的读数有效），再 G2-c（它让「停没停」可判定），
最后才是 G2-b（它只在真的停住时才有用）。

**不建议先做的**：不要先关 `pcie_thread` 或先改 PCIE 步数。`wifi10/11`（36/48 s 自回）与
`wifi12/13`（未自回）的 `pcie=` 只差 12 与 13，而 `wifi1`（`pcie=9`，195 s 自回）也是同一族。
`pcie` 步数**不是**这三轮的唯一差别，把它当单变量会得到一个假的因果。

## 8. 未验证

| 项 | 为什么 |
| --- | --- |
| `wifi12`/`wifi13` 是否真的停住 | 两轮都没有持久日志，只有控制台在 t≈13 s 后静默；静默也可能是 `qcom_usb_debug_write` 进了 `IO_FAILED`（`QcomXnuUsbDebug.c:1364`，见 `reference/archive/SMP-HUNT-2026-10-28.md:215` 记的 t=35 s/t=110 s 静音） |
| `a6xx2` 的 225 s 自回是哪条路径 | 没有 `boot_log_reset_result` 文本；`[reset]` 段只有 mark/pon/watchdog 三行 |
| `btn5` | 无日志，只能引用 `progress/2026-10-31.md:17` 的当面确认（用户断电） |
| `bt4` 的 `latest.txt` 里 `PK` 行是什么 | 是 `[profile]` 段的 PC 直方图，不是停住位置；它之所以在文件末尾，是因为 slot 文本先写 `[page]` 再写 `[profile]` |
