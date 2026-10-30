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

## 4.2 第二次执行记录：D1 不够，还缺 prelink roots（2026-10-31 05:19–05:26）

**这是本轮最重要的发现。D1 单独做不到。**

### 事实

按 §4.1 做完（defconfig 加 `-force_load`、`unixnu defconfig`、预检 `.config` 是我的值、构建），
`archive-25-build-alioth-usb` 跑过并 PASS，**然后链接失败**：

```
ld: symbol(s) not found for architecture arm64
  "__ZN21IOUSBDeviceControllerD2Ev", referenced from:
      __ZN15AliothUSBDeviceD2Ev in libAliothUSBDevice.a(AliothUSBDevice.o)
  "__ZNK20IOUSBDeviceInterface24getAlternateSettingCountEv", referenced from:
      __ZN15AliothUSBDevice34provideEndpointIDsForConfigurationEP7OSArrayPh
  "__ZNK21IOUSBDeviceController10getAddressEv", referenced from:
      __ZTV15AliothUSBDevice
  "__ZTV21IOUSBDeviceController", referenced from:
      __ZN15AliothUSBDevice5startEP9IOService in libAliothUSBDevice.a
NOTE: a missing vtable usually means the first non-inline virtual member function has no definition.
```

**`IOUSBDeviceController` 的 vtable 和方法都不在链接输入里。**

### 原因：`IOUSBDeviceFamily.kext` 没有 prelink 进内核

`board.toml` 的 `[kernel.prelink] roots` 是 **31 条，零 USB**：

```
$ for k in IOUSBDeviceFamily AppleEmbeddedUSB AppleUSBDeviceMux AppleSynopsysOTGDevice; do
    sed -n '151,200p' board.toml | grep -c "$k"; done
0 0 0 0
```

我们的子类继承 `IOUSBDeviceController`，而那个类**从来没被预链接进内核**，所以：

- 链接期：`__ZTV21IOUSBDeviceController` 未定义 → 失败（本轮实测）
- 即使绕过链接：运行时也没有类可以继承

### 所以 A-5 的入口是**两步**，不是一步

| 步 | 做什么 | 属主 | 越界 |
| --- | --- | --- | --- |
| **1** | `[kernel.prelink] roots` 加 `com.apple.iokit.IOUSBDeviceFamily`（及其依赖自动展开） | 设备仓库 `board.toml` | 否 |
| **2** | `CONFIG_XNU_EXTRA_LD_ARCHIVES` 加 `-Wl,-force_load,...libAliothUSBDevice.a`（§4.1 的 D1） | 设备仓库 `configs/` | 否 |
| 3 | 关 `qcom-usb-debug` | 设备仓库 `board.toml` | 否 |

**第 1 步必须在前。** 顺序反了会得到本轮的结果：档案构建成功、链接报未定义符号。

### 第 1 步的写法

`board.toml` 的 `roots` 是**整表覆盖**（`scripts/unixnu/profiles.py` 的 `merge()` 对列表
替换整表），所以**必须重复现有全部 31 条**再加一条。这一点 2026-10-30 的
`docs/evidence/2026-10-30-usb-stack-architecture.md` 已经记过，本轮实测再次确认。

### 第 1 步的判据（已实测，`nm` 那条是错的）

**不要用 `llvm-nm | grep IOUSBDeviceController`。** 实测它返回 0，**即使 kext 已经在内核里**：

```
llvm-nm kernel.macho | grep -c IOGPUFamily    → 0     (已知在内核)
llvm-nm kernel.macho | grep -c IOHIDFamily    → 0     (已知在内核)
```

原因：prelinked kext 的类符号**不进 `nm` 的符号表**，除非有对象主动引用它们
（`nm` 里那 48 个 `IOHIDDevice` 符号来自 `ShengHidInput` 的引用，不是 kext 自身导出）。

**正确判据：解析 `__PRELINK_INFO` 的 bundle 列表。**

```python
# 见 §7 复现脚本；核心是
pre = unserialize(__PRELINK_INFO)
ids = [e['CFBundleIdentifier'] for e in pre['_PrelinkInfoDictionary']]
assert 'com.apple.iokit.IOUSBDeviceFamily' in ids
```

**实测结果（2026-10-31 05:34）：**

```
prelinked bundles: 40
USB-related: ['com.apple.driver.usb.AppleUSBCommon',
              'com.apple.iokit.IOUSBDeviceFamily']
```

**判据通过。** 而且依赖被自动展开了：只加了 `IOUSBDeviceFamily` 一条，
`AppleUSBCommon` 作为它的 `OSBundleLibraries` 依赖一起进来了——这印证了
`kits/XnuPrelink/native_prelink.py` 的 `plan()` 会沿依赖展开。

**这条要在第 2 步之前单独验**，因为它验的是「类在不在内核里」，
与「我们的驱动进没进」是两件事。

---

## 4.3 第一次执行记录：被并发构建打断（2026-10-31 04:24–04:35）

**没有验成。** 记录在这里，因为踩到的坑比结论更有用。

### 时间线

```
04:24:54  改 configs/alioth_defconfig（加 -force_load）
04:25     第一次构建 → 失败于 drivers/ShengHidInput submodule 未初始化
04:28     git -c protocol.file.allow=always submodule update --init（在 orb 上）
04:30:51  .config 被另一个会话重写回旧值   ← 关键
04:31:35  archive-25-build-alioth-usb 跑过并 PASS
04:31:44  kernel.log 写出（链接成功，但用的是旧 defconfig）
04:35     第二次构建停在 archive-21，随后 btn 会话接管同一棵树
```

### 判据结果

| 判据 | 结果 |
| --- | --- |
| `nm -u` 为 0 | **过** |
| `nm | grep -c AliothUSBDevice` | **0 → 失败** |
| `nm | grep -c native_legacy_surface_stride` | **2，与改前相同** |

`mach.development.bcm2837.linkarchives` 里 13 条 `force_load`，**没有一条是我们的**，
连 `libLinkStubs.a` 都不在——证明 `EXTRA_LD_ARCHIVES` 那次构建没有生效。

### 根因：共享 `out/` 树只有一个 `.config`

`out/alioth/<os_profile>/alioth/.config` 是**所有会话共用**的一份。我 04:30 刷新了它，
04:30:51 被另一会话覆盖回旧值。构建于是用了没有 `-force_load` 的版本。

**这和「u275 的 `run=0` 是假信号」是同一类问题**，只是这次发生在构建期而不是运行期。

### 拿到的一条结果

`archive-25-build-alioth-usb` **确实跑过并通过**：

```
BUILT .../libAliothUSBDevice.a  (234 vtable slots, 18 pure overrides verified)
```

**这证明 `build-alioth-usb.py` 在真实构建树里能跑、自校验能过。**
但它只证明「档案能构建」，**不证明「档案能链接」**——那次链接用的还是旧 defconfig。

### 恢复

```sh
git checkout -- configs/alioth_defconfig
./unixnu defconfig alioth_defconfig -D ../UniXNU-alioth-usb
```

验证过 `grep EXTRA_LD_ARCHIVES` 两边都回到 `"{tree}/build/link-stubs/libLinkStubs.a"`。
**没有留下未验证的改动。**

### 重做的前提

**必须独占 `out/` 树**，否则同一个覆盖会再发生一次。两种方式：

1. **排队**：等别的会话构建结束（推荐，不引入新变量）
2. **独立 `--out`**：未验 `--out` 是否被 `build kernel` 接受；独立树要重新 prelink 并可能重拷 IPSW 缓存，磁盘只有 11 G

---

## 4.4 第 2 步执行记录：D1 链接失败，根因是架构顺序（2026-10-31 05:38–05:50）

**D1 做完，仍然失败，而且失败原因不是 D1。**

### 事实

roots 已含 `IOUSBDeviceFamily`（第 1 步验过），defconfig 已加 `-force_load`（快照验证过是我的值），
`archive-25` PASS，**然后链接报同样的未定义符号**：

```
ld: symbol(s) not found for architecture arm64
  "__ZTV21IOUSBDeviceController", referenced from: __ZN15AliothUSBDevice5startEP9IOService
  "__ZN21IOUSBDeviceControllerD2Ev", referenced from: __ZN15AliothUSBDeviceD2Ev
  "__ZNK20IOUSBDeviceInterface24getAlternateSettingCountEv", ...
```

### 根因：链接在 prelink 之前

`scripts/unixnu/kernel.py` 的顺序：

```
:203  build_archives(ctx, env)          ← 构建 archive
:204  run(['bash', script], ... 'kernel.log')  ← 链接内核本体
:207  def prelink(ctx):                 ← 之后才 prelink 合并 kext
```

**内核链接发生在 prelink 之前。** 那时 `IOUSBDeviceFamily.kext` 还不在任何链接输入里，
所以 `IOUSBDeviceController` 的符号在链接期**根本不存在**。

**加 roots 让 kext 进了最终镜像，但没让它参与内核链接。** 这两件事是不同的阶段。

### 那其它驱动怎么绑定 prelinked kext

**用「late archive」+ 绝对地址绑定**，不是普通符号引用。

`board.toml:112-130` 记着两个先例：

- `libNativeLegacyDisplay.late.a`：对 `IOMobileFramebufferLegacy` 的**绝对绑定**
- `NativeHidInput.late.o` + `hid-original-bindings.o`：对 `IOHIDDevice` 的绑定

`scripts/build-sheng-hid-input.py:105-107` 的机制：

```python
run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-c',asm,
     '-o',OUT/'hid-original-bindings.o'])          # 从 .S 汇编生成
run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=...', obj, late])          # 转成 .late.o
```

**`hid-original-bindings.o` 把 prelinked kext 的符号写成 `.quad 0xfffffff0...` 绝对地址**，
因为那些地址在 prelink 后是固定的。

### 所以 `AliothUSBDevice` 现在的方式做不到

它用**普通 C++ 继承**（`class AliothUSBDevice : public IOUSBDeviceController`），
编译器会生成对 `__ZTV21IOUSBDeviceController` 的**重定位引用**，链接器要求符号存在。
**而 prelink 的 kext 符号在链接期不存在。**

三条可能的出路，都需要先验证：

| 出路 | 做什么 | 风险 |
| --- | --- | --- |
| **A. 绝对地址绑定** | 新增 `build-alioth-usb-bindings.py`，从 `.o` 的重定位表 + 固件 vtable 自动生成 `.S` | **C++ 源码不改**（见下方更正）；新脚本照 `build-sheng-hid-input.py` 的形状 |
| **B. 反向：不继承，改用运行时查表** | 不写 `class X : public Y`，改为 `OSMetaClass` 运行时按名查找并手工填 vtable | 放弃 C++ 继承的全部便利，工作量最大 |
| **C. 让 kext 参与内核链接** | 把 `IOUSBDeviceFamily.kext` 的可执行段作为普通 archive 放进链接 | 会与 prelink 阶段重复定义；且 Apple kext 不是按这个方式设计的 |

**A 是最可能的，而且有现成先例。而且它不需要改一行 C++。**

### 更正：C++ 源码不用改

我在初稿里写「`AliothUSBDevice.cpp` 要重写成不继承的形状，A-3 全部代码都要改」——**那是错的。**

`scripts/build-sheng-hid-input.py:85-108` 的机制是**全自动**的：

```python
# :89  从固件的原始 kext vtable 解出每个槽的实际地址
compiled = [relocations.get(vt+16+i*8) for i in range(207)]
# :104 关键一步：只绑定「符号表里确实未定义」的那些
undefined = {n for n, _, k in symbols if k == 1 and '11IOHIDDevice' in n}
require(undefined == set(bindings), 'All and only actual original HID imports bound')
# :105 生成 .S
asm.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n, a in sorted(bindings.items())))
```

**第二步 `k == 1` 是「未定义符号」的标记。** 所以流程是：

1. C++ 源码照常写 `class X : public Y`（继承不变）
2. 编译出 `.o`
3. **读 `.o` 的重定位表，列出它实际引用了 Y 的哪些符号**
4. 从固件 vtable 解析这些符号的真实地址
5. 生成 `.S`，用 `.set` 把它们钉成绝对地址

**C++ 源码一行不用改。** 新增的是一个 `build-alioth-usb-bindings.py`，形状照
`build-sheng-hid-input.py`，产物是 `AliothUSBDevice.late.o` + `alioth-usb-bindings.o`，
再照 `build-gop-legacy-display.py` 的方式归档成 late archive。

**A-3 的 18 个纯虚实现、vtable 校验、宿主测试全部保留。** 只有「怎么进内核」这一层换成绑定。

### 判据（第 2 步的真实形态）

```
链接成功（无未定义符号）
llvm-nm -u kernel.macho | wc -l          → 0
nm | grep -c AliothUSBDevice             → >= 39（链接成功之后才有意义）
```

### 下一轮从哪开始（纯离线，不需要板子）

1. 读 `build-sheng-hid-input.py:85-108` 的完整形状，确认它的输入是「原始 kext 的 vtable 重定位」
   而不是「我们自己的 .o 的重定位」——**这两者的方向要确认**，因为 HID 那个脚本比对的是
   固件里 `IOHIDDevice` 的 vtable，不是它自己编出来的 `.o`。
2. 在固件里定位 `IOUSBDeviceController` 的 vtable（已实测 `0xfffffff006e81da0`，234 槽）
   和 `IOUSBDeviceInterface`（`IOUSBDeviceInterface` 的类名串在 `0xfffffff0054790c2`）。
3. 编出 `AliothUSBDevice.o`，读它的 `__const` 重定位，列出**实际引用**了基类的哪些符号。
4. 判断：这些符号能否全部从固件 vtable 解出绝对地址。

**第 1 条是这一轮必须先确认的**：如果那个脚本绑定的是「固件里 kext 自己的 vtable」，
而我们需要的可能是「我们自己 .o 里对基类的引用」——方向不同，脚本形状要跟着变。

### 已实测：链接器报的 4 个符号，地址来源**不统一**

| 符号 | 地址来源 | 状态 |
| --- | --- | --- |
| `__ZTV21IOUSBDeviceController` | **vtable 本体** `0xfffffff006e81da0`（A-1 实测） | ✓ 可直接 `.set` |
| `__ZNK21IOUSBDeviceController10getAddressEv` | **vtable 槽 171** = `0xfffffff00617db10` | ✓ 槽内容实测为 `ldrh w0,[x0,#0x17c]; ret`，正是 getAddress |
| `__ZN21IOUSBDeviceControllerD2Ev` | **不是 vtable 槽** | ✗ 槽 0/1 实测是 `brk #1`，不是析构；要从别处取 |
| `__ZNK20IOUSBDeviceInterface24getAlternateSettingCountEv` | **不是 `IOUSBDeviceController` 的槽** | ✗ 是另一个类 `IOUSBDeviceInterface` 的方法，代码地址 `0xfffffff00616fa0c`（字节匹配实测） |

**所以「从 vtable 补」只覆盖一半。** 另外两个要从**代码地址**取：

- `D2Ev`：析构函数是代码，不在 vtable 槽里。地址要从 `IOUSBDeviceFamily` 的
  `__TEXT_EXEC` 里找，或用 kernelcache 的符号（但 kext 符号不在 kernel symtab，
  这就是为什么 A-1 只能靠槽内容识别）。
- `getAlternateSettingCount`：已在 21H461 用**字节匹配**定位到 `0xfffffff00616fa0c`
  （见 A-2 的访问器验证），所以它的地址是已知的。

**这意味着 `build-alioth-usb-bindings.py` 不是「纯 vtable 驱动」的**，
需要一个「vtable 槽 + 已知代码地址」的混合表，和 `build-sheng-hid-input.py:103`
的「手写种子 + vtable 补齐」形状一致——但种子里要包含 `D2Ev` 这一类。

### 完整清单：`.o` 实际引用 20 个 `IOUSBDeviceController` 符号

链接器只报了 4 个（它遇到第一批就停）。`llvm-nm -u AliothUSBDevice.o` 列出**全部 20 个**：

```
allocateRequest  createPipeGated  handleUSBResume  getDeviceAddress
handleUSBSuspend  activatePipeGated  deactivatePipeGated  getMaxSpeedCapability
handleUSBCableConnect  handleUserlandCommand  handleUSBConnectionDone
handleUSBCableDisconnect  createControlRequestTracker  io
handleControlRequestStateChange  getAddress
+ __ZTV21IOUSBDeviceController  + __ZN21IOUSBDeviceControllerC2EPK11OSMetaClass
+ __ZN21IOUSBDeviceControllerD2Ev  + __ZN21IOUSBDeviceController10gMetaClassE
```

**16 个能直接从 vtable 槽取**（已实测槽号与地址）：

| 方法 | 槽 | 地址 |
| --- | --- | --- |
| `createPipeGated` | 172 | `0xfffffff00617d978` |
| `activatePipeGated` | 173 | `0xfffffff00617da04` |
| `deactivatePipeGated` | 174 | `0xfffffff00617dad4` |
| `getAddress` | 171 | `0xfffffff00617db10` |
| `handleUserlandCommand` | 178 | `0xfffffff00617ba9c` |
| `handleUSBSuspend` | 195 | `0xfffffff006181b08` |
| `handleUSBConnectionDone` | 197 | `0xfffffff006182518` |
| `handleUSBResume` | 198 | `0xfffffff006182734` |
| `handleUSBCableConnect` | 204 | `0xfffffff00618285c` |
| `handleUSBCableDisconnect` | 205 | `0xfffffff006181f14` |
| `getMaxSpeedCapability` | 207 | `0xfffffff00617de78` |
| `getDeviceAddress` | 209 | `0xfffffff00617e434` |
| `handleControlRequestStateChange` | 216 | `0xfffffff006180860` |
| `createControlRequestTracker` | 224 | `0xfffffff00617bc84` |
| `io` | 226 | `0xfffffff00617e8b8` |
| `allocateRequest` | 228 | `0xfffffff00617eb10` |

**4 个必须从别处取**，而且按重定位类型分两类：

| 符号 | 重定位类型 | 需要什么 |
| --- | --- | --- |
| `__ZTV21IOUSBDeviceController` | `GOT_LOAD_PAGE21/OFF12` | **数据地址** = vtable 本体 `0xfffffff006e81da0` ✓ |
| `__ZN21IOUSBDeviceController10gMetaClassE` | `GOT_LOAD_PAGE21/OFF12` | **数据地址**，指向元类对象；在 `__DATA` 里，待定位 |
| `__ZN21IOUSBDeviceControllerC2EPK11OSMetaClass` | **`BRANCH26`** | **代码地址**（构造函数） |
| `__ZN21IOUSBDeviceControllerD2Ev` | **`BRANCH26`** | **代码地址**（析构函数） |

**`BRANCH26` 是直接调用**（`bl` 指令），需要函数入口地址；**`GOT_LOAD` 是取数据指针**，
需要对象地址。这两类不能混。

### 4 个非 vtable 符号：全部已定位（2026-10-31）

| 符号 | 地址 | 怎么找到的 |
| --- | --- | --- |
| `__ZTV21IOUSBDeviceController` | **`0xfffffff006e81da0`** | A-1 实测（vtable 本体） |
| `__ZN21IOUSBDeviceControllerC2EPK11OSMetaClass` | **`0xfffffff00617ba04`** | 反查：搜 `add x8,x8,#0xda0`（即存 vtable `0xe81da0`），唯一命中 |
| `__ZN21IOUSBDeviceControllerD2Ev` | **`0xfffffff00617ba24`** | 从子类析构链反查：`AppleSynopsysOTGDevice` 槽 0 → `0x652507c` → `b 0x65261ac` → `b 0x65374bc`（GOT stub）→ `GOT[0x120] = 0x617ba24` |
| `__ZN21IOUSBDeviceController10gMetaClassE` | **`0xfffffff007b1a4c8`** | `0x617ba30` 的 `getMetaClass` 返回这个地址（`adrp 0x7b1a000 + 0x4c8`） |

**构造函数与析构函数的结构已核实**（`0x617ba04`..`0x617ba2c`）：

```
0x617ba04  C2: stp/mov/bl OSMetaClassC2 ; adrp 0xe81000 ; add #0xda0 ; str [x0] ; ret
0x617ba24  D2: b 0x618990c            ← 尾调 OSMetaClass 析构
0x617ba28  brk #1                     ← 抽象类标记
0x617ba2c  brk #1
0x617ba30  getMetaClass: adrp 0x7b1a000 ; add #0x4c8 ; ret
```

**C2 存的正是我们 A-1 定位的 vtable `0xe81da0`**——这独立确认了 vtable 的归属。

### 所以绑定表是完整的 20 条

- **16 条**从 vtable 槽（槽号与地址见上表）
- **4 条**从上述固定地址
- 全部已实测，无待查项

**下一轮的工作从「查地址」变成「写脚本」。**

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
| ~~符号冲突~~ | **已排除**：39/39 在 `AliothUSBDevice` 命名空间内，与 LinkStubs 零重叠 |
| ~~LinkStubs 的非 weak 符号~~ | **已排除**：`LinkStubs.c` 9 个定义里只有 `native_legacy_display_probe` 非 weak，而它在 `#if !XNU_NATIVE_LEGACY_DISPLAY` 里、本板该开关为 `y`，built archive 里实测 0 命中。唯一真正存在的非 weak 是 `native_legacy_surface_stride`，它本来就被 GPU 路径调用、已经在内核里 |
| `EXTRA_LD_ARCHIVES` 混合普通/force_load 写法能否被 make 正确拆分 | 只测了两个纯 `-force_load` 的情形 |
| LinkStubs 加 `-force_load` 是否撞重复定义 | 取决于链接器，必须真跑 |
| `nm kernel.macho` 里能否看到 `AliothUSBDevice` | 驱动进了内核才有，要构建后验 |
| force_load 之后 `__mod_init_func` 是否被正确执行 | 要真机 |
