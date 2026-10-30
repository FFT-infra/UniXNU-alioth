# A-5 入口方案：用 `CONFIG_XNU_EXTRA_LD_ARCHIVES` 载入板级 USB 驱动

2026-10-31。**方案，未实施、未构建、未上板。** 设备仓库 `NEW/UniXNU-alioth-usb` @ `e9f04f5`。

---

## 0. 为什么需要这一步

A-3 的驱动 `AliothUSBDevice` 已经能编译、vtable 正确、宿主测试通过，但**它从来没有进过内核**：

- 它在 `board.toml` 的 `[kernel] archives` 里，所以**会被构建**
- 但链接来源只有 `CONFIG_XNU_*_ARCHIVE` 那几行 `-force_load` 和 `XNU_EXTRA_LD_ARCHIVES`
- **没有任何一条指向 `libAliothUSBDevice.a`**

而普通链接**不会**拉进没有符号引用的对象——驱动类没有任何调用者，所以即使写进 `EXTRA_LD_ARCHIVES` 也会被整个丢掉。

**所以 A-5 的第一件事不是关掉 CDC，是让驱动真的进内核。** 顺序反了会在没有日志通道的前提下调一个根本没加载的驱动。

---

## 1. 两条候选路径

| 路径 | 改哪 | 越界 | 风险 |
| --- | --- | --- | --- |
| **D. 改 `CONFIG_XNU_EXTRA_LD_ARCHIVES` 的值** | `configs/alioth_defconfig`（**设备仓库**） | **否** | LinkStubs 也要加 `-force_load`，可能撞 weak/strong 冲突（见 §3） |
| A. 改 `build-xnu-4k-baseline.sh` 加一行 | 基仓库 | **是** | 一行，语义最清楚 |

**`EXTRA_LD_ARCHIVES` 能带多个**——实测过：make 按词拆分，两个 `-Wl,-force_load` 成为两个独立参数。

`$(LD_KERNEL_ARCHIVES)` 在 `MakeInc.kernel` 里出现 **4 次**，全部不加引号展开：
`:228`（LTO 链接）、`:230`（非 LTO）、`:233`（debug）、`:238`（`link.filelist` 路径），
另有 `:263` 把它原样写进 `.linkarchives` 文件。**所以无论走哪条链接路径，多参数都能塞**——
这是它成立的原因，不是某一条的巧合。

---

## 2. D 路的具体写法

```diff
# configs/alioth_defconfig:98
-CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a"
+CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,{tree}/build/link-stubs/libLinkStubs.a -Wl,-force_load,{tree}/build/alioth-usb/libAliothUSBDevice.a"
```

### 一个必须一起做的步骤：重跑 `defconfig`

`configs/alioth_defconfig` **不是构建时直接读的**。`scripts/unixnu/config.py:160` 把选定的
defconfig 拷成 `out/alioth/<os_profile>/alioth/.config` 的**快照**，之后每次构建读那份快照
（`need_config()` → `self.config_path`）。所以改完 defconfig 必须：

```sh
./unixnu defconfig alioth_defconfig -D ../UniXNU-alioth-usb
```

否则改动不生效，而构建会**成功**——只是用的还是旧的 `EXTRA_LD_ARCHIVES`，驱动仍然不进内核。
这是最坏的一类失败：看起来做了什么，实际什么都没做。

实测当前快照与 defconfig 一致（`grep EXTRA_LD_ARCHIVES` 两边 MATCH），所以从现在开始改，
重跑 defconfig 是唯一让改动生效的途径。

### 判据

```sh
K=<tree>/build/xnu/obj/.../kernel

# 1. 未定义符号必须为 0。force_load 拉进一个没人引用的 archive 时，
#    若它带未解析的外部引用，失败会报「未定义符号」而不是「重复定义」，
#    两者修法完全不同。先查这个比事后读链接错误快。
#    基线：现有候选 nm -u 实测 0 条，所以「仍为 0」是可判定的。
llvm-nm -u "$K" | wc -l          # 期望 0

# 2. 类符号真的进内核了
llvm-nm "$K" | grep -c AliothUSBDevice        # 期望 >= 39（18 方法 + vtable + metaclass）

# 3. LinkStubs 的解析没变
llvm-nm "$K" | grep -c native_legacy_surface_stride   # 与改前相同
```

**第 1 条是这次补的，也是最先该跑的。**

### 回退

defconfig 改回一行，重建。无其它副作用。

---

## 3. D 路的真实风险（必须先验）

**`LinkStubs.c` 里的定义全是 `__attribute__((weak))`**：

```
package/LinkStubs.c:12-13   void native_legacy_display_probe(void)      ← weak
package/LinkStubs.c:21-22   __attribute__((weak)) void boot_progress_poll(void)
package/LinkStubs.c:34-35   __attribute__((weak)) int native_ram_block_gpu_addon(...)
package/LinkStubs.c:48-49   __attribute__((weak)) volatile uint32_t native_ufs_dpu_state;
```

**weak 的语义是「有强定义时让位」。** 而 `native_legacy_display_probe` 在
`package/LegacyDisplayProbe.cpp` 里有**强定义**（`build-link-stubs.py:6` 的注释明说：
「on, LinkStubs.c leaves native_legacy_display_probe() to the display archive」）。

所以给 `libLinkStubs.a` 加 `-force_load` **会强制拉进那个 weak 定义**，与 display archive 的强定义
同时进入链接。**结果取决于链接顺序**：可能正常（强定义胜出），也可能报重复定义。

**这是 D 路唯一的实质风险，而且我无法离线验证**——它取决于链接器行为，必须真跑一次构建。

### 这条风险已实测排除（2026-10-31）

不需要链接就能验：重复定义只可能发生在符号层面，所以比符号即可。

```
libAliothUSBDevice.a 导出  39 个符号
libLinkStubs.a       导出 325 个符号
两者重叠            0
```

而且我们的 39 个符号 **全部**在 `AliothUSBDevice` 命名空间内
（`grep -c AliothUSBDevice` = 39/39）：`__ZTV15AliothUSBDevice`、
`__ZTVN15AliothUSBDevice9MetaClassE`、18 个方法、4 个 metaclass 成员。
**没有任何裸名符号**（没有 `native_legacy_*`、没有 `boot_progress_poll`）。

所以与 LinkStubs 不可能撞，与内核 .o 也不可能撞——内核不可能定义 `AliothUSBDevice` 这个类。

**但结论仍然是「LinkStubs 不加 force_load」**，理由换成更准的一条：
不是「会撞」（实测不撞），而是「没必要」——它的符号被
`offline-component-load-probes.patch:79-80,185-186` 真实调用，普通链接就拉得进来；
给它加 force_load 只会多拉进几个没人用的 weak 定义，收益为零。

### 如果撞了，退路

只给我们的驱动加 `-force_load`，LinkStubs 保持普通链接：

```diff
-CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a"
+CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a -Wl,-force_load,{tree}/build/alioth-usb/libAliothUSBDevice.a"
```

LinkStubs 的符号**确实被内核引用**，这一点我核实了：
`NEW/UniXNU/patches/xnu/offline-component-load-probes.patch:79-80,185-186` 里
`extern void native_legacy_display_probe(void); native_legacy_display_probe();`，
而 `configs/alioth_defconfig:100-103` 的注释也说明「both symbols must resolve or the kernel
link fails」。**所以 LinkStubs 普通链接就能拉进来，不需要 `-force_load`。**
不动它，两边语义都不变。

**我倾向先试这个版本**——它最小、风险最低，而且 LinkStubs 本来就工作正常，没有理由动它。

---

## 4. 推荐

**先试 §3 末尾的最小版本**（只给驱动加 `-force_load`，LinkStubs 保持原样），理由：

1. 不越界（只改设备仓库的 defconfig）
2. 不动一个已知工作正常的东西（LinkStubs）
3. 语义清楚：`EXTRA_LD_ARCHIVES` 本来就是「额外要链接的 archive」

**如果这个版本链接失败**（例如 `EXTRA_LD_ARCHIVES` 的解析对混合写法有意外），再退回 A 路
（改基仓库脚本，一行 `-Wl,-force_load,$ALIOTH_USB_ARCHIVE`）。

---

## 4.1 D1 的可直接套用形式

### 唯一一行改动

`configs/alioth_defconfig:98`：

```diff
-CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a"
+CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a -Wl,-force_load,{tree}/build/alioth-usb/libAliothUSBDevice.a"
```

`{tree}` 占位符保留——`scripts/unixnu/kernel.py:18` 的 `xnu_env()` 会用 `str(ctx.tree)`
替换它。**不要写成绝对路径**，否则换构建目录就失效。

### 完整执行顺序

```sh
# 1. 改上面那一行
# 2. 让快照跟上（不改这步，构建会成功但用旧的 EXTRA_LD_ARCHIVES）
./unixnu defconfig alioth_defconfig -D ../UniXNU-alioth-usb

# 3. 确认快照真的变了
grep EXTRA_LD_ARCHIVES out/alioth/ipados-17.7.11-21H461/alioth/.config
#    必须能看到 -Wl,-force_load,{tree}/build/alioth-usb/libAliothUSBDevice.a

# 4. 构建
./unixnu build kernel -D ../UniXNU-alioth-usb
```

### 三条判据（按顺序跑，第一条最先）

```sh
K=<tree>/build/xnu/obj/DEVELOPMENT_ARM64_BCM2837/kernel

llvm-nm -u "$K" | wc -l                              # 1. 期望 0
llvm-nm "$K" | grep -c AliothUSBDevice               # 2. 期望 >= 39
llvm-nm "$K" | grep -c native_legacy_surface_stride  # 3. 与改前相同
```

第 1 条的意义：`-force_load` 拉进一个没人引用的 archive 时，若它带未解析的外部引用，
失败会报「未定义符号」而非「重复定义」——**两者修法完全不同**，先跑这条比事后读链接错误快。
基线：现有内核候选 `nm -u` 实测 **0** 条，所以「仍为 0」是可判定的。

### 回退

```diff
-CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a -Wl,-force_load,{tree}/build/alioth-usb/libAliothUSBDevice.a"
+CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a"
```

再跑一次 `./unixnu defconfig alioth_defconfig`，重建。
**回退不涉及基仓库、不涉及板子、不涉及其它会话。**

## 5. 与其它阻塞的关系

| 阻塞 | 状态 |
| --- | --- |
| **磁盘 2.5 GiB** | **当前第一位**。`build kernel` 跑不了，所以这个方案现在**无法验证** |
| CDC 双 owner | 本方案之后才处理 |
| IOLog 进持久日志 | **已完成**（`docs/evidence/2026-10-31-msgbuf-frozen-snapshot.md`），但冻结快照要一轮长跑才验得了 |

**顺序**：磁盘腾出来 → 本方案验通（`nm` 有 `AliothUSBDevice`）→ 关 `qcom-usb-debug` → 真机。

---

## 6. 未验证

| 项 | 为什么 |
| --- | --- |
| ~~符号冲突~~ | **已排除**：39/39 在 `AliothUSBDevice` 命名空间内，与 LinkStubs 零重叠 |
| ~~LinkStubs 的非 weak 符号~~ | **已排除**：`LinkStubs.c` 9 个定义里只有 `native_legacy_display_probe` 非 weak，而它在 `#if !XNU_NATIVE_LEGACY_DISPLAY` 里、本板该开关为 `y`，built archive 里实测 0 命中。唯一真正存在的非 weak 是 `native_legacy_surface_stride`，它本来就被 GPU 路径调用、已经在内核里 |
| `EXTRA_LD_ARCHIVES` 混合普通/force_load 写法能否被 make 正确拆分 | 只测了两个纯 `-force_load` 的情形 |
| LinkStubs 加 `-force_load` 是否撞重复定义 | 取决于链接器，必须真跑 |
| `nm kernel.macho` 里能否看到 `AliothUSBDevice` | 驱动进了内核才有，要构建后验 |
| force_load 之后 `__mod_init_func` 是否被正确执行 | 要真机 |
