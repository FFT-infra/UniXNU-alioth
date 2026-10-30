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

**`EXTRA_LD_ARCHIVES` 能带多个**——实测过：make 按词拆分，两个 `-Wl,-force_load` 成为两个独立参数
（`MakeInc.kernel:228` 里 `$(LD_KERNEL_ARCHIVES)` 不加引号展开）。

---

## 2. D 路的具体写法

```diff
# configs/alioth_defconfig:98
-CONFIG_XNU_EXTRA_LD_ARCHIVES="{tree}/build/link-stubs/libLinkStubs.a"
+CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,{tree}/build/link-stubs/libLinkStubs.a -Wl,-force_load,{tree}/build/alioth-usb/libAliothUSBDevice.a"
```

### 判据

1. `build kernel` 成功（链接无重复定义错误）
2. `nm kernel.macho | grep AliothUSBDevice` 有输出——**类符号真的进内核了**
3. `nm kernel.macho | grep -c 'native_legacy_display_probe'` 与改前相同（LinkStubs 的解析未变）

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
| `EXTRA_LD_ARCHIVES` 混合普通/force_load 写法能否被 make 正确拆分 | 只测了两个纯 `-force_load` 的情形 |
| LinkStubs 加 `-force_load` 是否撞重复定义 | 取决于链接器，必须真跑 |
| `nm kernel.macho` 里能否看到 `AliothUSBDevice` | 驱动进了内核才有，要构建后验 |
| force_load 之后 `__mod_init_func` 是否被正确执行 | 要真机 |
