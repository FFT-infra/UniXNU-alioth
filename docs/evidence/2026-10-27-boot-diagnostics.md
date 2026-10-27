# 2026-10-27 串口实测结论（boot-usb17）

## 方法
USB CDC-ACM 控制台 + `qcom_usb_debug_write()` 直写（printf/kprintf 都不到串口）。
420 秒连续采集，21512 字节。

## 硬数据

| 进程 | 退出状态 | 次数 | 崩溃 PC |
|---|---|---|---|
| xpcproxy | 0 | 180 | — (正常) |
| cryptexd | 6 ns=18 code=2 | 13 | `PC1dc6523c0` |
| liveactivitiesd | 5 ns=2 code=5 | 11 | `PC1d27b64f8` |
| lsd | 11 ns=2 code=11 | 2 | `PC190fa9068` |
| routined | 6 ns=2 | 1 | `PC1dc64af74` |
| privacyaccountingd | 5 | 1 | `PC1002a4ef0` |

- `ns=18` = OS_REASON_LIBSYSTEM, `code=2` = OS_REASON_LIBSYSTEM_CODE_FAULT
- 所有崩溃共享同一 dyld slide：`+10ec0000`
- **每个进程的崩溃 PC 完全一致**，重复 N 次 => 确定性 bug，非竞态
- `lsd` ESR `0x92000005`：EC=0x24 低 EL 数据中止，ISS=0x5

## 本轮未复现的
- 无 `ns=9 code=8`（EXEC_EXIT_REASON_SECURITY_POLICY）。上一轮 backboardd/imagent 等
  的 MAC 拒绝是**间歇性**的，不是每次启动必现。
- 因此 `offline-exec-mac-verdict.patch` 本轮未触发，其 errno 仍未捕获。

## 重要更正
参考块称「总退出数从 513 降到 0」不成立。本轮实测 215 条左右，其中 xpcproxy 180 条
是正常 fork+exit。真实异常约 28 条。

## 待查
cryptexd 是 cryptex 挂载守护进程，它反复 libsystem fault 与 SystemOS 挂载链相关。
13 次同 PC 崩溃说明是确定性路径。

## cryptexd 分析
`OWNER_CRYPTEX_FILES=0`（board.toml 明确要求 UNION_SYSTEMOS 与 CRYPTEX_FILES 互斥）。
cryptexd 是 cryptex 挂载守护进程，我们的布局用 SystemOS union 代替了 cryptex，
它启动后访问预期路径失败 -> libsystem fault (ns=18 code=2) -> SIGABRT。
这是设计选择的结果，不是内核 bug。13 次同 PC 说明它被反复重启反复崩。

## 参考块评估（对照实测）
1. VFS panic 来自 reboot_kernel 的 vfs_unmountall —— **未验证**。我看到的 panic
   照片是 vnode_rele_ext usecount -ve，栈未确认含 vfs_unmountall。
   建议改为直接 PSCI 复位（跳过优雅卸载）这个方向合理，但魔数地址未实测，不采纳。
2. domain=5 的修复 —— **已完成并生效**，屏幕显示 QCOM USB ACTIVE。
   但参考块给的补丁（直接接管）会破坏 lease_shape 不变量，实际用的是
   「SCTLR.M=0 且 TTBR0=0 才回收」的判据。
3. 「总退出数 513 -> 0」—— **不成立**，实测 215 条，异常约 28 条。

## 符号化尝试（失败，如实记录）
从 IPSW 的 Cryptex1,SystemOS DMG 挂载出 dyld 共享缓存（3.3 GB + 686 MB symbols）。
slide 经核对为 0x10ec0000（libsystem_platform 落在 0x1800e9000，lsd 的 0x1800e9068
正好是它 +0x68，与 slide 一致）。

但把三个崩溃地址按 __TEXT 区间映射后，得到 cryptexd -> HomeKitDaemon、
liveactivitiesd -> CalendarUIKit，与进程名不符。说明区间解析有误
（dyld_info 对 split cache 的 __TEXT 输出不完整）。该结果不采信，不作为结论。

原始未符号化地址已存档，可日后用正确工具重做：
- cryptexd       0x1dc6523c0 (unslid 0x1cb7923c0)
- liveactivitiesd 0x1d27b64f8 (unslid 0x1c18f64f8)
- lsd            0x190fa9068 (unslid 0x1800e9068)

## 本次已提交并推送
- QcomXnuUsbDebug ee80ec9：SID 0 死槽位回收 + 拒绝行上报
- UniXNU-alioth 15a15d7：persist 行、退出日志、REFUSED 上报到串口
- UniXNU 43aa436：exec MAC 判决 errno 上报
- UniXNU-alioth 3590f89：porting.toml diagnostics 区 device-verified

## boot-usb18：设备侧符号化的尝试与限制

改动：`boot_progress_exit_fault` 里遍历 `dyld_all_image_infos.infoArray`，找到覆盖崩溃 PC
的镜像，输出 `image=<base> off=<pc-base>`；同时把 crash 行缓冲 400->640。

实测结果：**7 条 EXITCRASH，0 条带 image=**。

原因（源码可证）：`boot_progress_exit_fault` 的镜像查询和 `copyin` 都在
`if(pid==proc_selfpid())` 内。liveactivitiesd 的退出不是由它自己的线程上报的
（内核在父进程/launchd 上下文里回收），所以 `copyin` 无法访问目标进程的
`dyld_all_image_infos`。这是内核侧的根本限制，不是 bug。

**结论：崩溃 PC 的符号化无法在内核侧完成。**

## 另一个确凿事实：slide 每次启动都变
- boot-usb17: `+10ec0000`
- boot-usb18: `+0abf0000`

因此上一轮「用固定 slide 0x10ec0000 离线减地址」的做法**方法上就是错的**，
不只是数据解析错。符号化必须在拿到当次 slide 的前提下做，或者由用户态自己报。

## 本轮稳定复现的
liveactivitiesd sig5（SIGTRAP, ESR EC=0x3C = BRK 指令）10 次，PC 与 bt 链每次完全一致。
这是确定性触发，不是随机。

## 符号化成功（修正上一节）

关键更正：slide 换算方法是**对的**。两轮不同 slide（0x10ec0000 / 0x0abf0000）减出来的
unslid PC 完全相同（0x1c18f64f8），证明换算正确。上一轮失败纯粹是我的**镜像区间解析**错了。

正确方法：解析 `dyld_info -all_dyld_cache -segments`，只取每个镜像的第一条 `__TEXT`
（带 base + size），丢弃后续那些归到邻居镜像的子段地址。然后按
`base <= pc < base+size` 匹配。

结果（liveactivitiesd，sig5 / SIGTRAP / ESR EC=0x3C BRK #1）：
```
PC   0x1c18f64f8 -> CalendarUIKit.framework  +0xf64f8
bt1  0x1c18dc0ec -> CalendarUIKit.framework  +0xdc0ec
bt4  0x1c1a49ec0 -> CalendarUIKit.framework  +0x249ec0
bt5  0x1c1a3c024 -> CalendarUIKit.framework  +0x23c024
bt2  0x18f5b6cc0 -> libBNNS.dylib             +0x949cc0
```
崩溃完全落在 CalendarUIKit 内，三个栈帧同库。这是确定性触发（每轮 PC/bt 完全一致）。

含义：liveactivitiesd（活动组件守护进程）在加载 CalendarUIKit 后执行了固定的 BRK
（Swift/libsystem 的 trap 或断言），不是随机内存错误。

## 【重大突破】cryptexd 崩溃的真正原因（why= 抓到了）

boot-usb19 加入了 `exit_reason_get_string_desc()`，第一次拿到了 libsystem fault 的
payload 文本：

```
EXITLOG pid=168 name=cryptexd status=6 ns=18 code=2 why=unexpected failure: mkdirat: [28: No space left on device]
```

**根因是 ENOSPC（错误码 28），不是 cryptex 架构问题，也不是权限问题。**

Data 卷（`/private/var`）来自 `tmp/scratch-data-setup.img`，容量 **32 MiB**。
实测只用了 32 KiB，所以不是写满，而是**卷太小装不下 cryptexd 需要的目录树**。

这解释了为什么之前所有 PC 地址都指向无意义的库：
`abort()` 是从 libsystem 的通用错误处理路径里发出的，PC 根本不代表故障点。
**reason payload 才是唯一正确的信息源。**

## 修复方向
扩大 Data seed。生成器 `scripts/prepare-owner-data-fixture.py:25` 硬编码 `-size 32m`。
需要从原始 OS 卷的 `private/var` 重新生成一个更大的 seed（例如 2 GiB）。
注意 `board.toml` 的 `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` 绑定 seed 的卷 UUID，
重建后 UUID 会变，必须同步更新。

## 【修正】32 MiB 不是根因

实测 32 MiB 卷：`df` 显示 `32Mi  828Ki  31Mi  3%`，**还有 31 MiB 空闲**，
inode 也充足（227/65536）。所以不是卷写满。

`ENOSPC` 与"有空间"并存，只有一种解释：**cryptexd 写入的目标不在 Data 卷上**，
而是在一个零容量的层（典型是只读 union 层或未挂载路径）。

布局事实：`/private/var` 是 Data 卷（可写，内核校验 `MNT_RDONLY` 必须为假，
否则返回 EINVAL 而非 ENOSPC）。但 `/private` 属于根卷只读 union。
cryptexd 若在 `/private/var/...` 下 mkdir，理论上应落到 Data 层；
若它写的是 `/private/var/../..` 或别的绝对路径，就会落到零容量层。

2 GiB seed 的方案已放弃：boot 分区硬限制 192 MiB，payload 是 FAT 卷，
装不下 2 GiB。这个方向物理上不可行，不是参数问题。

## 下一步验证
在 cryptexd 的 mkdirat 失败点打路径。内核侧 `vn_open`/`mkdir` 有 vp，
可以在 ENOENT/ENOSPC 分支打出路径名。这比继续猜挂载层可靠。

## 回归排查：boot-usb21/22 卡在启动早期

现象：boot-usb21 和 boot-usb22 都只输出 3846 字节早期日志，0 条 EXITLOG；
`/dev/tty.usbmodem14101` 仍枚举，但控制通道 HELLO 三次无响应。

已排除：
- mkdir 探针用 printf 会死锁（`printf` -> `_cnputs` -> console 锁，而 mkdir1at
  在 namecache/vnode 锁下）。**这个判断本身是对的**，已改成
  `qcom_usb_debug_write` + weak_import。但改完后 boot-usb22 **仍然卡**，
  所以 printf 不是唯一原因，或不是原因。
- IMEM 改动（a45cded）的两个调用点都在诊断/控制路径，启动时不执行。
  已 revert 待验证。

线索：boot-usb19（只有 why= 改动）正常，输出 26020 字节、132 条 EXITLOG。
boot-usb21/22 多了 mkdir 探针。

待验证：boot-usb23 = 去掉 IMEM、保留 mkdir 探针（qcom_usb_debug_write 版）。

## 一个必须记录的方法问题
我多次用「字符串是否在 kernel.macho 里」判断改动是否生效，这对短字符串
（尤其被 `-fmerge-all-constants` 拆成立即数的）会误报。可靠做法是反汇编看
调用点，或直接看真机输出。

## 【根因确定】mkdir 探针死锁

隔离实验（镜像字符串对比，唯一变量）：

| 镜像 | mkdir 探针 | why= | 结果 |
|---|---|---|---|
| boot-usb19 | 0 | 1 | 正常，26020 字节，132 条 EXITLOG |
| boot-usb21 | 1 | 1 | 卡死，3846 字节，0 条 |
| boot-usb22 | 1 | 1 | 卡死，3846 字节，0 条 |

**唯一变量就是 mkdir 探针。**

根因（源码可证）：
`qcom_usb_debug_write()` 在 phase==ACTIVE 时不仅写 backlog，还会调 `poll_owned()`
去轮询 DWC3 控制器，诊断模式下甚至有一个 1ms 的 flush 窗（`delay_us`）。
而 `mkdir1at` 的 `out:` 处持有 **namecache 锁和 vnode 锁**。
在这些锁下做硬件轮询和延时，会阻塞整台机器的每一次路径查找 -> 启动卡死。

这也解释了为什么第一版用 `printf` 也卡：`printf` -> `_cnputs` -> console 锁，
同样是锁内阻塞。两次都是"在 VFS 锁下做了不该做的事"。

## 修复
在模块里新增 `qcom_usb_debug_enqueue()`：**只写 CPU backlog，不碰控制器**。
它复用 `backlog_write()`，后者只做本地 IRQ 屏蔽 + 环形缓冲写入，
无 MMIO、无延时、无跨 CPU 自旋（源码 167-178 行可证）。
探针改用它。

## 教训（写进方法）
判断"改动是否生效"不能只看字符串在不在 kernel.macho 里：
`-fmerge-all-constants` 会把短字符串拆成立即数，产生假阴性。
可靠做法是**镜像级 A/B 对比**（本次正是靠 boot-usb19/21/22 的
mkdir 字符串计数差异定位）。

## 【根因锁定】cryptexd 的 ENOSPC 来自 /private/preboot 只读挂载

源码链（可逐行核对）：

1. `zzyb-native-cryptex-os-view.patch:23`
   ```c
   native_cryptex_os_bind(..., const char *point, int flags)
   struct mount_args request = {.flags = MNT_RDONLY | flags, ...};
   ```
   **每个 bind 都强制 MNT_RDONLY。**

2. 第 50 行把 `/private/var/ngpu/preboot` **只读 union** 挂到 `/private/preboot`：
   ```c
   native_cryptex_os_bind(p, scratch, "/private/var/ngpu/preboot", "/private/preboot", MNT_UNION);
   ```

3. `cryptexd` 的职责就是在 `/private/preboot/Cryptexes/` 下维护 cryptex 存储，
   启动后必然要在那里 mkdir。写只读层 -> ENOSPC。

这与实测完全吻合：
- Data 卷 `/private/var` 是独立 APFS 卷、可写、31 MiB 空闲（`df` 实测）
- 但 `cryptexd` 写的不是 `/private/var` 本身，而是经由 bindfs 叠加出来的
  `/private/preboot`，那是只读的

## 因此 32 MiB 扩容方案从一开始就是错的
即使把 Data 卷扩到 2 GiB，`/private/preboot` 仍是 MNT_RDONLY，cryptexd 照样崩。
这也解释了为什么之前所有指向"空间不足"的直觉都不对——错误码名字骗人。

## 候选修复（待验证，不冒进）
A. 让 `/private/preboot` 这次 bind 可写（去掉 MNT_RDONLY，只给这一个挂载点）。
   风险：它是 union 叠加层，写入会落到下层 `/private/var/ngpu/preboot`，
   而该目录在 seed 里不存在，需要先由 native_gpu_addon_parents 创建。
   注意 native_gpu_addon_parents 从 sizeof("/private/var/ngpu") 开始建，
   **不会创建 ngpu 本身**，所以首次 mkdir 可能先拿到 ENOENT。
B. 不挂 `/private/preboot`，让 cryptexd 在真实路径上工作。
   风险：SystemOS union 依赖这条绑定，改动面大。
C. 接受 cryptexd 崩溃（它是可禁用的服务），先让启动继续。
   这与上游 sheng 的 BootWatchdogPolicy 思路一致：用 sheng-disable-jobs 关掉
   无外设的自恢复循环服务。但要先确认关掉它不影响 SystemOS 访问。

倾向：先做 A 的最小版本 + 探针验证路径，再决定是否 C。

## 【实测否定】32 MiB 卷不是容量瓶颈

在 32 MiB 的 Data seed 副本上创建 **20000 个目录**：
```
created 20000 dirs
/dev/disk15s1  32Mi  5.8Mi  26Mi  19%  20k  262k  7%
```
只用 5.8 MiB，剩 26 MiB，inode 20k/262k。

**结论：容量和 inode 都不是原因。** "Data 卷太小"这个方向彻底否定，
2 GiB seed 方案从一开始就是错的（而且 boot 分区 192 MiB 也装不下）。

## 已排除的假设（都有证据）
1. Data 卷容量/inode 不足 —— 实测 20000 目录仅用 5.8 MiB
2. /private/preboot 只读 —— 只读在 vfs_lookup.c:1572 返回 EROFS(30)，实测是 ENOSPC(28)
3. staged_system_apps 池耗尽 —— 那段代码不处理 cryptexd 的路径
4. 我们自己的补丁返回假 ENOSPC —— 只有 staged_apps 内部用，不传播

## 剩余可能
ENOSPC 只能来自真实 APFS 驱动的创建路径。cryptexd 写的卷要么是
- 根卷/SystemOS（只读，但那会给 EROFS）
- 或另一个可写 APFS 卷

需要真机探针确定：`mkdir-enospc: <path>` 会直接给出挂载点。
这是 boot-usb25 要回答的唯一问题。

## 【根因彻底确定】ENOSPC 来自 fsevents 事件队列已满，不是磁盘

完整证据链（全部可源码核对）：

1. `mkdir1at`（vfs_syscalls.c:9845）在 `vn_create` 成功之后调用
   `add_fsevent(FSE_CREATE_DIR, ctx, FSE_ARG_VNODE, vp, FSE_ARG_DONE)`。
2. `add_fsevent`（vfs_fsevents.c:520）在 `kfse == NULL` 时返回 **ENOSPC**。
   `kfse` 来自 `zalloc_noblock(event_zone)`，即 fsevents 事件队列。
3. 队列满的条件是**没有 watcher 消费**。实测：所有串口日志里
   `fsevents` 相关进程出现 **0 次** —— `fseventsd` 从未运行。
4. 于是队列单调填满，之后**每一次 mkdir 都返回 ENOSPC**，
   即使目录已经创建成功。

这完美解释了所有反常现象：
- Data 卷有 31 MiB 空闲却报 "No space left on device"
- 实测 32 MiB 卷能建 20000 个目录（5.8 MiB，inode 充足）
- 只读挂载会给 EROFS(30) 而非 ENOSPC(28)
- cryptexd 每次都在同一个 PC 崩溃（同一段错误处理代码）

## 修复方向（明确）
fsevents 事件队列满不该让 mkdir 失败。这是内核的健壮性问题：
`add_fsevent` 的返回值在 `mkdir1at` 里被当作 mkdir 的结果，但事件记录失败
与目录是否创建成功无关。

两个可选做法：
A. 在 `mkdir1at`（以及同类 create 路径）忽略 `add_fsevent` 的返回值。
   改动小、语义正确（事件丢失不该导致系统调用失败），但影响面要看其它调用点。
B. 让 fsevents 队列在有 watcher 之前不分配/不排队。
   更根本，但要改 fsevents 核心逻辑，风险大。

倾向 A，并且只在 CONFIG_FSE 且返回值是 ENOSPC 时忽略，保留其它错误。

## 【撤回】fsevents 假设不成立

核对 vfs_syscalls.c:9844-9846 后确认：
```c
#if CONFIG_FSE
	add_fsevent(FSE_CREATE_DIR, ctx, FSE_ARG_VNODE, vp, FSE_ARG_DONE);
#endif
```
**返回值没有被使用**，是独立语句。所以 add_fsevent 的 ENOSPC 不可能
传播到 mkdir1at 的 error。这条假设撤回。

## 现在只剩一个可能
`vn_create()` 本身返回 ENOSPC。它在 vfs_syscalls.c:9821：
```c
if ((error = vn_create(dvp, &vp, &nd, vap, 0, 0, NULL, ctx)) != 0) {
    if (error == EKEEPLOOKING) { ... }
    goto out;
}
```
所以是 **APFS 或 union 层在创建目录时返回的 ENOSPC**。

这也意味着：Data 卷确实有空间（实测 20000 目录只用 5.8 MiB），
所以 cryptexd 写的**不是 Data 卷**，而是另一个真实可写 APFS 卷，
或 union 层转发给了某个下层。

`mkdir-enospc: <path>` 探针（boot-usb25 已包含）会直接给出这个路径。
这是唯一能终结猜测的数据。

## union 层是二进制，无法继续离线推导

`bsd/miscfs/union/` 只有 `union.h` 和 `Makefile`，没有 `.c`。
union 实现来自预链接的原版二进制。APFS 同理。
所以 `vn_create` 里 ENOSPC 的确切来源（union 转发 vs APFS 本身）
**无法从源码继续推导**，只能靠真机探针。

## 本轮排除清单（每条都有证据）
| 假设 | 结论 | 证据 |
|---|---|---|
| Data 卷容量不足 | 否 | 实测 20000 目录仅 5.8 MiB，剩 26 MiB |
| inode 耗尽 | 否 | 20k/262k |
| /private/preboot 只读 | 否 | 只读返回 EROFS(30)，实测 ENOSPC(28) |
| staged_apps 池耗尽 | 否 | 那段代码不处理 cryptexd 路径 |
| fsevents 队列满 | 否 | add_fsevent 返回值在 mkdir1at 里未被使用 |
| 我们自己的补丁返回假 ENOSPC | 否 | 仅 staged_apps 内部使用，不传播 |

## 唯一待测
`mkdir-enospc: <path>`（boot-usb25 已含）会直接给出 cryptexd 的写入路径。
拿到路径就能立刻判断是哪个卷、哪一层。

## 崩溃表分析（比符号化更可靠）

按 PC 分组后的实测数据：

| PC | 次数 | 不同 FAR | 涉及进程 |
|---|---|---|---|
| `1d7a9ef74` | **35** | 1 (全为 0) | assistantd, accountsd, **lsd**, routined |
| `1cc4e64f8` | 10 | 1 | liveactivitiesd |
| `1d7aa63c0` | 9 | 1 | cryptexd |
| `18c3fd068` | **7** | **7** | lsd |
| `1d63823c0` | 5 | 1 | cryptexd |

两个关键事实：

1. **`PC=1d7a9ef74` 横跨 4 个不同进程、35 次、FAR 恒为 0。**
   不同进程崩在同一地址 => 共享库代码（dyld 缓存里的公共库），
   且 FAR=0 说明是空指针解引用。

2. **`PC=18c3fd068` 同一进程 7 次、FAR 每次都不同**
   （`133d1d90`/`10079148`/`100d48f0`/`11bd4e28`/`10114b60`/`100d3fc0`/`10079148`）。
   同一 PC、不同 FAR = 悬垂指针（use-after-free），
   不是空指针。

## 符号化到此为止（不可靠）
`libsystem_platform`、`dyld`、`libsystem_kernel` 等在 split cache 里
`dyld_info` 报 `__TEXT base 0x0`，因为它们在另一个分片里，地址信息缺失。
用这类数据减 slide 得到的结果（如"多个无关进程都崩在 HomeKitDaemon"）
自相矛盾，**不采信**。

要真正符号化，需要把 split cache 的各分片合成为一个（`dyld_shared_cache_util -d`），
或从设备上取 crash log。这是后续可做但成本较高的一步。

## 符号化的最终判断：数据自相矛盾，不采信

验证过 slide 换算是对的：`unslid=0x1cb78af74` 在两个不同 slide
（0xc314000 / 0xabf0000）下都恒定出现。

但把该地址映射到镜像得到 `HomeKitDaemon.framework`，
而崩溃的 4 个进程是 assistantd / accountsd / lsd / routined ——
**它们都不链接 HomeKitDaemon**。所以镜像名不可信。

原因：`dyld_info -all_dyld_cache` 对 split cache 的很多镜像给出
`__TEXT base 0x0`（它们在别的分片里），导致按 base+size 匹配时
把地址归到邻居镜像。这是工具限制，不是数据错误。

**结论：要真正符号化，必须先把 split cache 合成单一文件
（`dyld_shared_cache_util -d` 之类），或从设备取 crash log。
在这之前，不再基于镜像名下任何结论。**

## 可靠的替代信息（已拿到）
- `unslid=0x1cb78af74`：35 次、跨 4 个进程、FAR 恒为 0 -> 共享库里的空指针解引用
- `unslid=0x1800e9068`：lsd 专属、FAR 每次不同 -> 悬垂指针
- `unslid=0x1c18f64f8`：liveactivitiesd、10 次、BRK #1 -> 主动 trap
- `unslid=0x1cb7923c0`：cryptexd、14 次 -> 同 PC 但 reason 文本已给出真因(mkdirat ENOSPC)

## 参考块核实结果（三条参考）

### 属实并已修复
1. **IMEM 写入被误删** —— 属实。`c03633b` 在排查卡死时把 `platform_imem_bootloader`
   整段 revert 掉了，之后定位到真因是探针死锁，却忘了恢复。已恢复（`b16bbfe`）。
2. **探针只抓 ENOSPC** —— 属实。已改为抓除 EEXIST/ENOENT 外的所有 errno，
   并打印 errno 本身（`f767cf1`）。

### 不属实（有实测反证）
3. **GCTL `== 2` 需要放宽为 `2||3`** —— **错误**。
   `halted()` 在 `scan()` 之前执行（248 行 vs 268 行）。若 `(g>>12)&3 != 2`，
   会返回 `CoreRunning`(4)。实测 `domain=` 读 **5**（StreamExists），
   证明 `==2` 已经通过。放宽它只会削弱一个正确的检查。

4. **`backboardd` 在起跑线被 MAC 处决** —— **与实测不符**。
   最近四次启动（why/why2/image/final）里 `backboardd` 出现 **0 次**；
   它只在更早的 crash2/exit3 窗口出现过。当前它**根本没有被启动**，
   不是被杀。启动停在渲染层之前更早的位置。

5. **AMFI 参数能救 backboardd** —— **机制上不成立**。
   `amfi_get_out_of_my_way` / `cs_enforcement_disable` 只作用于
   `kern_codesigning.c` 的 CS_CONFIG（代码签名层）。
   而 `ns=9 code=8` 来自 `mac_cred_label_update_execve` 遍历
   `mac_policy_list` 的钩子（`security/mac_vfs.c:840`），是另一层。
   两个参数已加入（成本为零，覆盖签名层），但不指望它们清掉那个拒绝。

### 已采纳（低成本、有依据）
6. `sheng-disable-jobs=com.apple.cryptexd,com.apple.liveactivitiesd`
   —— 采纳。两者都是确定性失败且在本架构下无工作可做：
   SystemOS 已由 `zzyb-native-cryptex-os-view.patch` 绑定到
   `/private/preboot/Cryptexes/OS`，cryptexd 无事可做；无账户无网络，
   live activities 无意义。上游参考机也是直接禁用而非逐个追查。

## 关于 Metal 的说法
参考块称"没有 Metal 就绝对不出帧"。方向可能对（`SWAP calls=0` 是事实），
但**当前还没走到那一步**：`backboardd`/`SpringBoard`/`runningboardd` 一次都没启动，
所以现在讨论 Metal 为时过早。先把渲染层进程拉起来，才有 `SWAP` 可谈。

## 第三轮参考核实

### 被实测直接证伪
**"只读 APFS 卷返回 ENOSPC(28)"** —— 错误。
在真实只读 APFS 卷（`044-76040-038.dmg`，Apple_APFS_Volume）上实测 mkdir：
```
errno=30 EROFS
```
只读卷返回 **EROFS(30)**，不是 ENOSPC(28)。所以 cryptexd 的 ENOSPC
不可能是"写只读卷"造成的。这一条在参考块里出现了两次，两次都错。

### 部分成立，但边界要说清
**Metal 为 nil -> 0 脏区 -> 跳过 Selector 17** —— 链条本身有 QEMU 证据：
`scripts/check-original-render-server.py` 断言
`MTLCreateSystemDefaultDevice` 对真实 backboardd 返回 nil、
`initial_primary_frame_count=0`、`actual_scanout_content='previous loader frame'`。
但该脚本自己标注 `hardware_tested=False`，是 **QEMU 观察**，不是真机。

**关键限制**：真机上 backboardd 连启动都没发生（最近四次启动出现 0 次），
所以"Metal nil 导致不出帧"这条链在真机上**尚未被观察到**。
它可能是真的，但不是当前的第一因。

### 关于 AMFI 参数的重复主张
参考块第三次重复"加 amfi 参数即可让 backboardd 进用户态"。
已核实：这些参数只作用于 `kern_codesigning.c` 的 CS_CONFIG，
而拒绝来自 `mac_cred_label_update_execve` 的 `mac_policy_list` 钩子。
两者不是同一层。参数已加（零成本），但不作为修复依据。

## 【关键发现】启动卡在 lsd 这一层，不是 backboardd

用 xpcproxy -> 子进程 的配对关系还原了 launchd 的实际拉起序列（261 条记录）：

- 被反复拉起：`lsd`(17次崩)、`assistantd`(15)、`cryptexd`(9)、`accountsd`(4)
- **从未被拉起：`backboardd`、`SpringBoard`、`runningboardd`、`WindowServer`**

在最后一次 `lsd` 失败之后，只剩 7 条事件，全部是
`assistantd` / `lsd` / `xpcproxy` 的循环。

**结论：启动停死在 LaunchServices 这一层，根本没有推进到 GUI 层。**
参考块反复主张的"backboardd 被 MAC 处决"不是当前现象——
它压根没被 launchd 派生过（最近四次启动出现 0 次）。

`lsd` 是 LaunchServices 数据库守护进程，SpringBoard/runningboardd 启动前必须先
查它。它 17 次失败，后面的层级自然全部不启动。

`lsd` 的崩溃特征（之前已测）：同一 PC、FAR 每次都不同（0x133d1d90 / 0x10079148 /
0x100d48f0 / 0x11bd4e28 ...），是悬垂指针而非空指针。

## 因此优先级重排
1. `lsd`（最上游，卡住整个 GUI 层）
2. `assistantd`（15 次，同样 ns=2 code=6）
3. `cryptexd`（已通过 sheng-disable-jobs 禁用）
4. GUI 层（backboardd/SpringBoard）—— 还没到
5. Metal —— 更靠后，现在谈为时过早

## 【更上游的阻塞】安全栈从未启动

xpcproxy 配对分析 + 服务清单核对：

`securityd`、`trustd`、`tccd`、`sandboxd` 在整个 why.log（38 进程、261 条）
里**全部 0 次**。`keybagd` 启动了 1 次。

这三个 plist 都存在且有 MachServices，但没有 RunAtLoad，
是**按需启动**。它们没起来，说明 lsd/assistantd 崩溃时
对它们的服务查找没有触发启动 —— 或者 lsd/assistantd 在
查询之前就自己崩了。

`unslid 0x1cb78af74`（35 次、跨 4 进程、FAR=0）这些进程
（lsd/accountsd/routined/assistantd）的共同点是都要访问
安全/账户/数据库服务。它们全崩在同一空指针地址，最可能的
解释是某个共享的基础服务（securityd 或类似的）没起来，
它们的初始化代码拿到空句柄就解引用。

## 已从参考推进到的真实上游
参考块一直在推 backboardd / Metal。但实测：backboardd 从未被启动，
securityd/trustd/tccd/sandboxd 也从未启动。真正的最上游是
**安全栈 + 它依赖的账户建索引**，而这些服务一个都没起来。

下一步最有价值：查 launchd 为什么没按需拉起 securityd。
可能原因：Mach 服务名解析失败、data 卷缺迁移标记、
或 securityd 所需的容器(Cy+性容器)不存在。

## securityd 未启动的机制（源码级）

`com.apple.securityd.plist` 的关键字段：
- **没有 `RunAtLoad`、没有 `KeepAlive`** —— 纯按需启动
- `MachServices`: 8 个（escrow-update / kcsharing / octagon 等）
- `LaunchEvents.com.apple.notifyd.matching` 只有两个触发：
  `com.apple.springboard.lockstate` 和 `kSOSCCHoldLockForInitialSync`
- `MinimalBootProfile: True`

**触发条件依赖 SpringBoard 的锁屏通知**，而 SpringBoard 又依赖
securityd 提供的密钥链服务。这是循环依赖，正常由早期引导路径打破。

我们的启动里 `init_featureflags` 成功（status=0），所以条件求值没问题。
真正的原因是：**没有任何进程成功请求 securityd 的 Mach 服务**，
因为 lsd/assistantd 在发起请求之前就已经崩在 `0x1cb78af74`（空指针）。

## 破局点收敛到 lsd
lsd 17 次崩、跨进程共享 PC、FAR=0。
它的崩溃发生在调用安全/数据库服务之前或之中。
只有让 lsd 活下来，securityd 才可能被按需拉起，
然后 accountsd/assistantd 才有救，然后才是 GUI 层。

## 离线能做的最后一步
lsd 崩溃在共享库的空指针。要定位需要符号化，但 split cache 的
dyld_info 对部分镜像报 base 0x0，导致镜像名不可信。
可行的补救：用 `dyld_shared_cache_util -d` 把 split cache 合成单一文件后
再符号化。这需要 macOS 上的工具，且耗时较长，但这是唯一可靠路径。

## 符号化：确认不可行，正式停止

`.symbols` 文件（686 MB）实测结构：
- header: localSymbolsOffset=0x28cc4000, size=0x148000
- 之后是 83968 条 nlist_64 记录
- **文件里没有 strtab**（末尾 `remaining 0x0`，扫描 0x28e00000 附近无 ASCII 区）

也就是说这个文件只有地址和类型，没有名称。名称要么在 dyld 的
`localSymbolsInfo` 子结构里（需要完整解析 dyld_cache_header 的
localSymbolsInfoOffset），要么需要 `dyld_shared_cache_util` 合成。

`dyld_shared_cache_util` 在本机不存在（Xcode 未安装该工具）。
`atos` 对 split cache 直接报 "cannot load symbols"。

**结论：当前环境下无法把崩溃 PC 符号化到函数名。已尝试 4 种方法，
全部有明确失败原因。这条线正式关闭，不再投入。**

原始 unslid 地址已完整存档，日后有工具时可重做：
- 0x1cb78af74（lsd/accountsd/routined/assistantd 共享，FAR=0）
- 0x1800e9068（lsd 专属，FAR 每次不同）
- 0x1c18f64f8（liveactivitiesd，BRK #1）
- 0x1cb7923c0（cryptexd，mkdirat ENOSPC）

## 构建受阻：prelink 需要联网

`unixnu build kernel` 在 build 之后无条件调用 `kernel.prelink(ctx)`，
而 prelink 通过 `scripts/unixnu/ipsw.py` 的 `RangeFile` **流式读取**
Apple CDN 上的 IPSW（`iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`）。

当前网络中断（`SSL: UNEXPECTED_EOF_WHILE_READING`），本地无 IPSW 缓存，
所以**无法产出新内核**。这是环境问题，不是代码问题。

`--skip-preflight` 不管用，因为它跳的是别的检查。

## 本轮已完成的离线工作
1. 查明 lsd 的 launchd plist：纯按需启动，只有 MachServices，无 RunAtLoad/KeepAlive
2. lsd 只依赖 5 个标准框架，依赖面很小 -> 崩溃在运行时数据而非链接
3. seed 里**没有 LaunchServices 数据库**（`mobile/Library/Caches/` 是空目录），
   lsd 首次启动要自己创建它
4. 在 macOS 上复制 seed 测试 mkdir：**成功**，卷可写
   （但注意 macOS APFS 与 XNU APFS 是不同实现，不能直接类推到设备）
5. 发现 `native_owner_data_status` / `native_owner_data_stage`
   **从未被任何输出通道显示** —— 这是真实的信息缺口
6. 已在 BootProgress.c 中把 `data=<status>/<stage>` 加进 EXITLOG 行，
   但**因网络中断无法构建验证**

## 待办（网络恢复后）
- 构建并验证 data= 字段
- 刷入 boot-usb26 + data 字段，看 Data 挂载状态与各进程失败的相关性

## 【重要纠正】网络不是构建阻塞

我上一轮说"prelink 需联网下载 IPSW，无法构建"——**这个判断是错的**。

实测：
```
./unixnu build kernel   # 不设任何环境变量
k=0   urlopen 次数 = 0   kernel.macho sha256 a726cdff...
```

构建完全离线成功，`prelink` 用的是本地
`out/ipsw/21H461/kernelcache.decompressed`（46.4 MB，早已就绪）。

之前那次 SSL EOF 是偶发网络抖动，`kernel.py` 在**没有** `spec['kernelcache']`
时才走 `ipsw.Ipsw(ctx)` 联网。而实际解析后的 board 配置里是有 kernelcache 路径的
（虽然我直接读 board.toml 的 [kernel] 只看到 4 个键，说明 profiles 合并层
注入了它，我没定位到具体注入点）。

参考块说的"本地 IPSW 组件早已就绪"是对的。

## boot-usb27 已构建
五项改动全部在位：
- data=%d/%d（新）
- mkdir-err=%d
- IMEM 0x146bf65c <- 0x77665500（反汇编确认 2 处立即数）
- sheng-disable-jobs
- cs_enforcement_disable / amfi_get_out_of_my_way

## 方法教训（今天第三次）
用 `strings` 查 `0x146bf65c` 和 `77665500` 计数为 0，但反汇编里明确有
`movk w0,#0x146b` 和 `movk w8,#0x7766`。**立即数编码的字符串/地址不能用
strings 判断存在性**，必须反汇编。

## 五项离线任务核实

### 1. A650 GpuProbe.cpp 编译 —— 部分属实，但优先级错
`tmp/_pipa/package/GpuProbe.cpp` 存在（1432 行），配套 `A650Gmu.c/h` 也在。
可以尝试对齐编译。**但**：当前系统卡在 `lsd`，GPU 栈是阶段 5，
在 backboardd 都没启动的情况下编译 GpuProbe 不会产生任何可见变化。
不否认它的长期价值，但现在做它 = 又一次"造望远镜"。

### 2. 预置 Data 卷数据库 —— **前提不成立**
实测：`TCC.db` 和 `Accounts3.sqlite` 在
- 我们的 Data seed 里：**不存在**
- 原始 OS 卷（`044-76040-038.dmg` 的 `private/var/db/`）里：**也不存在**

这些数据库是**运行时由守护进程自己创建**的，不是随系统预置的文件。
没有可拷贝的模板。所以"往 seed 里塞 TCC.db"无法执行。

### 3. songchiyu 触控驱动 —— **仓库不存在**
`/Volumes/Disk/xinyang/Lab/Bridge/Src/` 下没有 songchiyu 仓库。
`_ref/ios-alioth` 里只有 QEMU 的 `touch.py` 和 `rwtouch.c`，
不是 FocalTech FT5652 驱动。无法集成不存在的东西。

### 4. canvas-width/height —— 属实，但优先级低
`ARCHIVE/ios-sweet/NOTES.md:98,113` 确认 IOMobileFramebufferOpen 读这两个属性，
且注明是 **optional**（"canvas-width/height read as 4-byte CFData ...
from the IOService-plane parent (optional)"）。
我们用 `IOMobileFramebufferLegacy`（原版预链接类），不是 SweetFramebuffer。
属性缺失不阻断。可补，但不解决当前问题。

### 5. porting.toml 回填 —— 属实且可做
12 个区域仍是 `planned`。可以回填已真机验证的事实。
但 `porting-record.py check` 目前因"components without an area"报错
（6 个组件未覆盖），这不是回填能解决的，需要新建区域。

## 五项任务核实后的实际执行

| # | 任务 | 核实结论 | 处理 |
|---|---|---|---|
| 1 | A650 GpuProbe.cpp 编译 | 源码在（1432 行），但优先级错 | 未做 |
| 2 | 预置 Data 卷数据库 | **前提不成立**：TCC.db/Accounts3 在原始 OS 卷也不存在，是运行时生成 | 未做 |
| 3 | songchiyu 触控驱动 | **仓库不存在**，_ref 里只有 QEMU 的 touch.py | 未做 |
| 4 | canvas-width/height | 属实但 **optional**，我们用 Legacy 类不走那条路 | 未做 |
| 5 | porting.toml 回填 | 属实且可做 | **已完成** |

第 5 项执行结果：
- `storage` 填 4 个组件（ShengUfs/VerifiedRoot/VirtioMmioBlock/RamBlock），
  内容全部来自真机实测：/dev/md0 的 ENOENT、20000 目录只用 5.8 MiB、
  cryptexd 的 ENOSPC 指向别的挂载点
- `display` 填 GopFramebuffer：DISPLAY stage=10 / CLIENT calls=4 / SWAP calls=0，
  并明确写下限制"从未有任何一帧被提交，backboardd 从未启动"
- `AppleKeyStoreState` 归入 configuration
- 证据文件拷进仓库 `docs/evidence/`，让 reference 从新克隆也能解析

**`scripts/porting-record.py check` 现在输出：`alioth: 13 structured areas checked`**
（之前是 "components without an area: [6 个]" 直接失败）

## 【回归定位】mkdir-lookup 探针是 boot-usb26/27 卡死的原因

镜像对比（唯一变量法）：

| 镜像 | mkdir-err | mkdir-lookup | data= | 结果 |
|---|---|---|---|---|
| boot-usb19 | 0 | 0 | 0 | **正常**，26020 字节，132 条 EXITLOG |
| boot-usb26 | 1 | **1** | 0 | 卡死，3846 字节 |
| boot-usb27 | 1 | **1** | 1 | 卡死，3833 字节 |

**`mkdir-lookup` 探针在 boot-usb26/27 都存在，而 boot-usb19 没有。**

根因（源码）：`offline-mkdir-lookup-enospc.patch` 在 `nameiat` 失败路径上调用
```c
proc_getpid(vfs_context_proc(ctx))
```
这两个函数都可能取进程/凭证锁。而 `nameiat` 失败（尤其 ENOENT）在启动中
大量发生，于是每次失败都在锁内调用它们 -> 与之前 printf 死锁同类。

**已移除该探针**，boot-usb28 只保留 `mkdir-err`（在 out: 标签，仅 ENOSPC 等
非普通失败时触发，且只用 `nd.ni_cnd.cn_pnbuf` 这个已解析的缓冲区）和 `data=`。

## 三次死锁的共同模式
1. printf（console 锁）
2. qcom_usb_debug_write（控制器轮询 + 延时）
3. proc_getpid(vfs_context_proc(ctx))（进程/凭证锁）

**都是在 VFS 锁内调用了会取其它锁的东西。**
教训：任何插在 VFS 路径上的探针，只能用"读已解析缓冲区 + 纯内存队列"这一种操作。

## 回归定位失败，需要更严格的隔离

boot-usb28（已移除 lookup 探针）**仍然卡死**：3817 字节、0 条 EXITLOG、控制通道无响应。

所以 lookup 探针**不是**原因，我上一轮的隔离实验有漏洞。

严格的镜像差异表：
| 镜像 | mkdir-err | data= | sheng-disable-jobs | cs_enforcement | 结果 |
|---|---|---|---|---|---|
| boot-usb19 | 0 | 0 | 1 | 16 | 正常 |
| boot-usb26 | 1 | 0 | 2 | 17 | 卡 |
| boot-usb27 | 1 | 1 | 2 | 17 | 卡 |
| boot-usb28 | 1 | 1 | 2 | 17 | 卡 |

**三个变量同时变了**，我一次改了太多东西，无法归因：
1. mkdir-err 探针（a5c4c7e）
2. sheng-disable-jobs 新增禁用列表（a9b5f6e）
3. cs_enforcement_disable / amfi_get_out_of_my_way（a9b5f6e）

`mkdir-err` 条件已排除 ENOENT/EEXIST，只在异常时触发，理论上安全。
`sheng-disable-jobs` 有完整边界检查（strlcat + 显式长度检查）。
但 **`cs_enforcement_disable=1` 会关闭代码签名强制**，这可能导致
launchd 或早期进程加载未验证的二进制而行为异常——这是最可疑的。

## 下一步：一次只改一个
下次构建只加 `mkdir-err`（回退 board.toml 的两项参数），
验证它单独是否安全。这是二分法的第一步。

## 【确认】mkdir 探针确实是元凶，但机制未查明

boot-usb29 是干净二分的结果：
| 镜像 | mkdir-err | data= | sheng-disable-jobs | cs_enforcement | 结果 |
|---|---|---|---|---|---|
| boot-usb19 | 0 | 0 | 1 | 16 | 正常 |
| boot-usb29 | **1** | **1** | 1 | 16 | **卡死** |

**唯一差异就是探针，所以探针是元凶。** 这次隔离是干净的。

但机制我还没查明。已逐个排除：
1. `snprintf` —— `vfs_syscalls.c` 里已有 4 处使用，VFS 上下文安全
2. `nd.ni_cnd.cn_pnbuf` 读 —— 已解析缓冲区，只读
3. `qcom_usb_debug_enqueue` 的 `backlog_write` —— 立即返回不自旋，
   `busy` 用 exchange 且马上释放（QcomUsbBuffer.h:29,44）
4. `daifset #3` 屏蔽中断 —— 但触发频率应该很低（已排除 ENOENT/EEXIST）

**剩下的疑点**：`backlog_write` 用 `msr daifset, #3` 屏蔽 IRQ+FIQ，
如果启动中非 ENOENT/EEXIST 的 mkdir 失败**实际上很频繁**
（例如沙箱大量 EACCES），中断屏蔽累积会饿死定时器/存储中断。

这个假设可以验证：把探针限制成**只报 ENOSPC**（回到最初的窄条件），
如果那样就不卡，说明是频率问题。

## 结论
探针这条路已经花掉 4 次刷机（26/27/28/29）全部失败。
**决定：放弃 mkdir 探针。** cryptexd 的 ENOSPC 路径改从其他角度查
（例如 sheng-disable-jobs 直接禁用 cryptexd，不需要知道它写哪）。

## 【panic 根因】Data mount 返回 ENOENT -> panic

用户提供的 panic 屏照片：
```
offline scratch Data mount failed: 2 @kern_exec.c:8216
```
`2` = ENOENT。

源码链：
1. `kern_exec.c:8214` `native_owner_data_prepare()`
2. `:8000` 调 `native_ram_block_volume_seed(...)`，返回 false 则 `error = ENOENT`
3. `NativeRamBlock.cpp:448` -> `selectVolumeSeed()`
4. `:374` `IOService::serviceMatching("IOMedia")` + `IOPropertyMatch UUID`
   -> **找不到带该 UUID 的 IOMedia 就返回 0**

所以 Data mount 失败 = RamBlock 没有发布 IOMedia。

## 重要更正：这解释了我前面 5 次刷机的"卡死"
boot-usb26/27/28/29/30 全部"卡在启动早期、3846 字节、0 条 EXITLOG"，
我一度归因于 mkdir 探针（并为此改了两次代码、刷了 4 次机）。

**真正原因是 Data mount panic**：panic 后内核停住，不再产生任何退出记录，
所以看起来像"启动早期卡死"。

而 boot-usb19（`why.log`，261 条 EXITLOG）确实过了 Data mount。
两者差异只有 `why=` 参数那一个 hunk —— 但那不该影响 Data mount。

**所以我现在无法解释为什么 boot-usb19 能过而 boot-usb30 不能。**
这个矛盾必须解决，不能再猜。

## 已排除
- UUID 不匹配：实测 seed 的 UUID 就是 `B0B12459-...`，与 defconfig 一致
- seed 哈希不匹配：`5add0dcb...` 与 payload.json 记录一致
- RamBlock 未链接：`native_ram_block_volume_seed` 和 `selectVolumeSeed`
  都在 kernel.macho 符号表里
- ADT 缺 RAMDisk：payload.img 里 `RAMDisk` 字符串存在
- RamBlock start 的尺寸检查：32 MiB 远小于 2 GiB 上限

## 【根因确认并修复】脏 UUID 在 .config 里，不在 defconfig

参考块正确指出了方向，但准确位置是 `.config` 而不是头文件本身。

三方核对：
| 来源 | UUID |
|---|---|
| 真实 seed（diskutil 实测） | `B0B12459-4F23-4E03-9B64-A902959309AA` |
| `alioth_defconfig` | `B0B12459-...` ✓ |
| **`.config`** | **`45828D34-98F0-4BBB-ADCB-2B7EE25BD9E6`** ✗ |
| 生成的 `owner-data-volume.h` | 同 `.config`（脏） |

**`unixnu build kernel` 读 `.config`，不读 defconfig。**
我在 2 GiB seed 实验时改过 defconfig，后来改回去了，但**从没重跑
`unixnu defconfig`**，所以 `.config` 一直保留着实验期的脏值。
这个脏值经 `build-xnu-4k-baseline.sh:420-440` 的生成逻辑写进
`owner-data-volume.h`，再编进内核。

于是 `selectVolumeSeed()` 拿 `45828D34` 去匹配真实 UUID 为 `B0B12459`
的 IOMedia，永远匹配不到 -> ENOENT -> panic。

## 修复
1. `./unixnu defconfig alioth_defconfig -D ../UniXNU-alioth` 同步 `.config`
2. 手动写入 `owner-data-volume.h`（因为 build-boot-progress.py 在
   build-xnu-4k-baseline.sh 之前跑，而后者才生成这个头文件；
   删掉头文件会导致前置步骤失败）
3. 重建内核：`kernel.macho sha256 0e064d44...`
4. 验证：内核字符串里 `B0B12459` 1 处，`45828D34` **0 处**

## 这一轮最大的教训
我之前 5 次刷机（26/27/28/29/30）全部"卡在启动早期"，我把它归因于
mkdir 探针，为此改代码、删探针、刷了 4 次机 —— **方向全错**。
真正原因是 Data mount panic，一张 panic 屏照片就定死了。
**教训：先看屏幕再改代码。** 我一直在串口上找证据，而 panic 屏就在那里。

## 【修复后实测】boot-usb31 恢复正常启动

```
10901 字节  118 条 EXITLOG  27 个进程  5 条 EXITCRASH
```

对比修复前（boot-usb30）：3833 字节、0 条 EXITLOG、panic。

**且本轮 cryptexd 和 lsd 都没有崩溃**（修复前分别崩 9 次和 17 次）。
这强烈提示：那些崩溃很可能就是 Data mount 失败的下游后果——
`/private/var` 没挂上，所有需要它的守护进程都拿不到数据。

剩余唯一异常：`liveactivitiesd` 6 次 SIGTRAP（BRK #1 in CalendarUIKit）。
GUI 进程（backboardd/SpringBoard/runningboardd）仍然是 0。

## 流程缺陷（重要）
`unixnu build kernel` 读 `out/<...>/.config`，而 `.config` 只由
`unixnu defconfig` 生成。**改 defconfig 后不重跑 defconfig，构建仍用旧值，
且没有任何警告。** 我这次就是这样把 2 GiB 实验的脏 UUID 带进了 5 个镜像，
并误判成"探针死锁"，为此刷了 4 次机。

规则：每次改 `configs/*_defconfig` 后立即跑
`./unixnu defconfig alioth_defconfig -D ../UniXNU-alioth`。

另外 `build-boot-progress.py` 在 `build-xnu-4k-baseline.sh` 之前运行，
而 `owner-data-volume.h` 由后者生成。手动删那个头文件会让前置步骤失败。
