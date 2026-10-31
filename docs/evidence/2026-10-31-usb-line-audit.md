# USB 那条线的真实状态：链接在 prelink 之前，绝对绑定没有真值来源

## 顺序

`scripts/unixnu/kernel.py`：

```
:226  build_archives(ctx, env)                        ← 构建 libAliothUSBDevice.a 等
:227  run(['bash', script], … 'kernel.log')           ← 链接内核本体
:254  def prelink(ctx)                                ← 之后才把 kext 合并进来
```

**内核链接那一刻，`IOUSBDeviceController` 的 vtable 与符号还不存在**——它们在 prelink
之后才被合并。`AliothUSBDevice.o` 里有 **55 个** 对 `IOUSBDeviceController` 的未定义引用，
链接期一个都解析不了。

所以这条线从一开始就不是「加个归档」能成的，它靠**绝对地址绑定**：把每个符号绑到它在
最终镜像里的固定地址，绕开链接期的符号解析。

## 但绑定表没有真值来源

`scripts/build-alioth-usb-bindings.py` 的 `TABLE` 有 18 条，`check_table()` 只验**形状**：

| 验的 | 不验的 |
| --- | --- |
| 地址是十六进制 | **地址对不对** |
| 落在内核窗口 `[KERNEL_LO, KERNEL_HI)` | 那个槽是不是真的指向这个方法 |
| 按种类对齐（代码 4 字节、数据 8 字节） | |
| 槽号在 0..233 | |

所以「`PASS: 20 bindings`」只证明形状合法。**`build-sheng-hid-input.py` 不是这样做的**：
它拿一个 reference candidate Mach-O，用 `require(i<168 and names.get(name)==methods[i])`
（`:105`）逐槽比对，并且写进 `abi-audit.json` 的 `runtime_qualified` 是 `False`——
**它自己承认没在真机上验证过。**

## 缺的 37 个

`nm -u AliothUSBDevice.o` 的 55 个里，表覆盖 18，缺 37：

- **26 个 `nativeSlot<N>`**：槽号在 `package/IOUSBDeviceController.h` 里逐行标了，
  地址 = `0xfffffff006e81da0 + 16 + N*8`。**这 26 个有确定来源。**
- **11 个其它**：`createPipe`、`activatePipe`、`deactivatePipe`、`freeRequest`、
  `readyForHost`、`getDMACommand`、`setProperties`、`handleUSBReset`、
  `callPlatformFunction`、`getAddress`、`__ZTV21IOUSBDeviceController`。
  **它们的槽号只能从内核里的 vtable 读出来。**

## 真值来源是什么

试过三条，都不行：

| 来源 | 结果 |
| --- | --- |
| kernelcache 的 `LC_SYMTAB`（6056 个符号） | **0 个** `IOUSBDeviceController`——预链接把 kext 符号剥掉了 |
| `__PRELINK_INFO` | 只有 personalities（`IOConfiguration-ncmBringup` 等），没有符号表 |
| 全文件搜 `__ZN21IOUSBDeviceController` | **0 处** |

**所以真值只能来自板子的运行时读数**：`a5-entry-plan.md` 里那些地址就是那么来的
（从 vtable 槽实测）。这也是 `abi-audit.json` 的 `runtime_qualified` 该被置为 true 的地方。

## 结论与选项

| # | 做法 | 代价 |
| --- | --- | --- |
| 1 | **把这 37 个地址从板子上读出来**，补齐表，接上 `CONFIG_XNU_EXTRA_LD_ARCHIVES` | 一轮上板（读 vtable 需要板子），加一行 defconfig |
| 2 | 先只接 26 个 `nativeSlot`（槽号确定），看链接还缺什么 | 一轮构建，会失败在 11 个上 |
| 3 | 整条线先停，把 `board.toml` 的假注释删掉、归档保留但不接 | 零成本；USB 设备模式仍然不可用 |

**这一轮我选 1**，因为槽号公式与基址都有，只差从板子读 11 个槽的内容。
**但 11 个的槽号要从 vtable 里找，那需要知道每个方法在 vtable 的哪一格**——
这个可以在板子上遍历 `0xfffffff006e81da0` 的 234 个槽，把它们与 `IOUSBDeviceController.h`
里的声明顺序对齐。

**下一步（离线可做）**：写一个探针模式，打印 vtable 每格的值，与头文件里 234 行声明逐行对齐，
就能定出 11 个的槽号。

---

## 补：表里存的是「槽的内容」，不是槽的地址

`build-alioth-usb-bindings.py` 里 `getAddress` 那条：

```python
('__ZNK21IOUSBDeviceController10getAddressEv', 0xfffffff00617db10, CODE, 171, 'slot content'),
```

槽 171 本身的地址是 `0xfffffff006e81da0 + 16 + 171*8 = 0xfffffff006e82308`，**而表里存的是
`0xfffffff00617db10`**——那是槽里**指向的代码**。

所以：

| 已知 | 未知 |
| --- | --- |
| 26 个 `nativeSlot<N>` 的**槽号**（头文件逐行标了） | 每个槽里**指向哪段代码** |
| 7 个有槽号的方法（`createPipe` 168 … `getDMACommand` 230） | 同上 |
| 槽号公式 `0xfffffff006e81da0 + 16 + N*8` | 槽内容是运行时的值，**只能从板子读** |

**结论：34 个缺失符号里有 33 个能定出槽号，但一个都定不出地址。** 剩下的 3 个
（`setProperties`、`callPlatformFunction`、`readyForHost`）连槽号都没有：
前两个是 `IOService` 的 override（槽在 0..167，槽号要从 `IOService` 的 vtable 查），
`readyForHost` 是**非虚的 Family 入口**（`IOUSBDeviceController.h:133`），根本不在 vtable 里。

## 所以这一步必须在板子上做

在探针里加一个模式，把 `0xfffffff006e81da0` 起的 234 个槽逐格读出来，打印 `槽号: 值`。
然后：

1. 26 个 `nativeSlot<N>` 直接按槽号取；
2. 7 个有槽号的同理；
3. `setProperties`/`callPlatformFunction` 从 `IOService` 的 vtable（需要先知道它在哪）；
4. `readyForHost` 要从调用它的地方反查（像 `D2Ev` 那样，`a5-entry-plan.md` 记过这条方法）。

**这一轮不做上板**（板子留给 GPU 与 WiFi 那两条），所以 USB 线的收口是：
**表保持 20 条、`archives` 保持关闭**，把上面这段写进 evidence，等下一轮补。
