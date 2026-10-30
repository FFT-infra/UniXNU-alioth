# A-3 验收：什么真的能用、什么还是 stub、真机上先看什么

2026-10-31。设备仓库 `NEW/UniXNU-alioth-usb` @ `usb-work`，提交 `bfd9903` 为 A-3 的终点。
**离线验证，未上板。**

这一节是为了 A-5：关掉 CDC 之后没有串口日志，判断「驱动到底加载了没有」只能靠这里。
所以每个判断都写明它的依据层级。

---

## 1. 结论

**A-3 的自然终点是「所有不碰控制器寄存器的方法都真的能用」。现在到了。**

| 类别 | 数量 | 方法 |
| --- | --- | --- |
| 纯虚方法总数 | 18 | |
| **真的实现** | **2** | `createIOBuffer`、`provideEndpointIDsForConfiguration` |
| **正确常量/空（有意）** | 6 | `connectedWithAccessoryCable`(false 是正确答案)、`getConnectionSpeed`(常量 1，真值要读 `DSTS`)、`prepareConfiguration`、`handleSetAddressPre/PostStatusPhase`、`nativeSlot175`/`nativeSlot199` |
| **仍是 stub** | 10 | 全部要碰控制器，是 A-5 的边界，见 §4 |

---

## 2. 真的实现了什么，依据是什么

### 2.1 `createIOBuffer`（槽 189）

```cpp
if (capacity == 0 || capacity > 1024 * 1024) return nullptr;
return IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options,
                                                   (vm_size_t)capacity, 4096);
```

| 依据 | 层级 | 出处 |
| --- | --- | --- |
| 1 MiB 上限 | **固件实测** | `AppleNubSynopsysOTGDevice` 槽 189 的 `cmp x2,#0x100,lsl #12` → 超限返回 null（`syn.dis:0x653673c`） |
| 页对齐 | **本板实测** | `QcomXnuUsbDebug/usb/Dwc3.h` 的 `DWC3_DMA_ALIGNMENT` 与 SMMU 映射都是 4 KiB |
| 不走 `KMA_LOMEM` | **本板实测** | arena 是静态数组（`QcomXnuUsbDebug.c:37`），物理地址走 `ml_static_vtop`（`:705`），从不经 `kmem_alloc_contig`；`vm_lopage_needed = FALSE`（`osfmk/vm/vm_resident.c:163`） |

**这一条排除了 ios_sagit 的坑 3。** 他们的 32 位 DMA 陷阱在我们这条分配路径上不可达。

### 2.2 `provideEndpointIDsForConfiguration`（槽 185）

完整实现：遍历 `OSArray` → 每个 `IOUSBDeviceInterface` 的 alt 数 → 每个 alt 的 pipe 数 →
每个 pipe 的 `getDirection()` → 按方向逐一编号 → append 进 `ids`。

| 依据 | 层级 | 出处 |
| --- | --- | --- |
| 算法（IN/OUT 各自从 1 起、同接口 alt 复用编号、按最大值推进） | **源码** | ios_sagit `QcomUSBDevice.cpp:968-999` |
| `ids` 按物理端点索引，上限 32 | **本板实测** | `Dwc3Regs.h:55` `DWC3_ENDPOINTS_NUM 32` |
| 端点号 4 位，15 是最后一个 | **规范** | USB 2.0 端点地址位宽 |
| **alternatives 是 append 不是取最大** | **源码 + 宿主测试** | ios_sagit 的单一 `index++`；`alioth_usb_endpoint_host.c` 锁住 2+4=6 |
| 实例大小 312 / 120 | **本板实测** | OSMetaClass ctor `mov w3,#0x138` / `#0x78` |
| 四个访问器只读 | **本板实测** | 指令字节逐字节匹配 + 反汇编确认无 store |

**宿主测试覆盖**（`package/tests/alioth_usb_endpoint_host.c`，`cc -std=c11 -Wall -Wextra -Werror`）：
IN/OUT 从 1 起、alt 复用且按最大值推进、第二接口接第一个之后、15 可用而 16 被拒、
pipe 上限、空参数、失败不写入、**append 累加**、方向值非 0/1 被拒、`in+out > pipe` 被拒。

**它抓到了两个真 bug**，都在写的时候没看出来：
1. 上限按 16 写，拒掉合法的 15（`EP_COUNT=6` 是 CDC 驱动自己的数组大小，不是硬件上限）
2. 失败的 setting 留下半截写入 → Family 会发布半填的配置

> 上面表格里 `getConnectionSpeed` 归在「正确常量」而不是「真的实现」，理由见 §3：
> 返回 1 在 A-3 阶段是安全的，但它不是协商速率。

---

## 3. 有意空实现（不是遗漏）

| 方法 | 槽 | 为什么空着是对的 |
| --- | --- | --- |
| `prepareConfiguration` | 186 | Apple 的实现要「先停用所有已启用端点，再重启端点配置」——**要碰控制器**。Family 仍持有配置，`activateEndpoint`（也是 stub）才会用它。空着不产生错误状态。 |
| `getConnectionSpeed` | 184 | 返回 1（full speed）在 A-3 阶段安全：Family 拿它做 pipe 过滤，最低档不会拒绝任何合法配置。真值要读 `DSTS.CONNECTSPD`，那是 A-5。 |
| `handleSetAddressPostStatusPhase` | 183 | DWC2 驱动上它就是 `{return 0;}` 桩（`syn.dis:0x652e2f0` 两条指令）。**但 DWC3 上它可能要写 `DCFG`/`DEPCMD`**，没核实之前空着比写错安全。 |
| `handleSetAddressPreStatusPhase` | 182 | 同上 |
| `handleStallEndpoint` / `handleAbortEndpoint` | 180/181 | 要写 `DEPCMD` |
| `connectedWithAccessoryCable` | 179 | 返回 `false` 是**正确实现**，不是 stub：普通主机连接必须为假（ios_sagit 坑 2）。 |
| `nativeSlot175` / `nativeSlot199` | 175/199 | 21H461 上它们的 C++ 名未复原，语义未知。空实现保证 vtable 形状正确。 |

---

## 4. 仍是 stub 的（A-5 的边界）

| 方法 | 槽 | 卡在哪 |
| --- | --- | --- |
| `goOnBus` | 177 | 要写 `DCTL.RUN_STOP`，控制器被 `qcom-usb-debug` 占着 |
| `activateEndpoint` | 187 | 要写 `DEPCMD`（`Dwc3.c` 的 `configure_ep`） |
| `deactivateEndpoint` | 188 | 同上 |
| `isEndpointStalled` | 192 | 要读 `DEPCMD` 完成状态 |
| `resetEndpointDataToggle` | 193 | 要写 `DEPCMD` |
| `doRemoteWakeGated` | 198 | 要写 `DCTL` |
| `ioGated` | 219 | 要提交 TRB 到 `DEPDMA` |
| `getConnectionSpeed` | 184 | 返回常量 1；真值要读 `DSTS.CONNECTSPD` |
| `nativeSlot175/199` | 175/199 | 语义未知 |

**共同原因**：`qcom-usb-debug=1` 时 CDC 控制台独占控制器。两个 owner 同时写寄存器会互相破坏，
而真机上会呈现为「我们的驱动没反应」——无法归因。**A-5 关掉 CDC 才能做这些。**

---

## 5. 真机上第一次能观察到什么

按可观测性从强到弱：

### 5.1 强信号（直接证明驱动加载了）

| 现象 | 在哪看 | 说明 |
| --- | --- | --- |
| `AliothUSBDevice` 出现在 IORegistry | `ioreg -c AliothUSBDevice`（若有用户态工具） | 类名匹配 = 驱动加载 |
| `USBDeviceEndpointIDs` 属性 | 同上，或 `usbctl registry AliothUSBDevice` | **只有 `provideEndpointIDsForConfiguration` 成功返回才会设**。这是 A-3 唯一一条能自己产生的可观测输出。 |
| Family 的配置器日志 | 持久日志 `msgbuf.txt` | `IOUSBDeviceConfigurator: controller->createUSBDevice()=0x%08x`（`IOUSBDeviceFamily` 字符串表）。**这条证明 `start` + `readyForHost` 走通了。** |

### 5.2 弱信号（可能被别的因素干扰）

| 现象 | 说明 |
| --- | --- |
| `AppleSynopsysOTGDevice::start : registers at %p, %p physical` | 那是 **Apple 的 DWC2 驱动**，不是我们的。**看到它说明 Apple 的驱动抢到了，我们的没抢到** |
| 主机枚举到 `05ac:12ac` | 需要 A-5 全链路，A-3 做不到 |

### 5.3 判据（A-5 用）

**A-3 的代码真的加载了** 当且仅当：
1. 持久日志里出现 `IOUSBDeviceConfigurator: controller->createUSBDevice()` 的结果行，且
2. IORegistry 里有 `AliothUSBDevice`，且
3. **没有** `AppleSynopsysOTGDevice::start : registers at` 那一行

第 3 条是排除项：出现它说明 Apple 的 DWC2 驱动匹配上了，我们的没有。

**注意**：这三条都要等 A-5 关掉 CDC 才有日志通道。而关掉 CDC 的前提是 §IOLog 那条
（`docs/evidence/2026-10-31-msgbuf-frozen-snapshot.md`）——冻结快照保证 boot 期的 IOLog
在长跑后仍可读。

---

## 6. 构建期的自校验（每次构建都跑）

`scripts/build-alioth-usb.py` 拒绝归档，除非：

- vtable 恰好 **234 槽**（21H461 的 `IOUSBDeviceController` 布局）
- slot 0 = `AliothUSBDeviceD1Ev`、168 = `createPipe`、233 = `nativeSlot233`
- 18 个纯虚槽全部由 `AliothUSBDevice` 填充

**这比「编译通过」强**：头文件一旦和固件对不上，构建期就失败，而不是产出一个加载后
每条调用都错位的驱动。

当前构建输出：

```
BUILT .../libAliothUSBDevice.a  (234 vtable slots, 18 pure overrides verified)
```

---

## 7. 没有验证的（必须真机）

| 项 | 为什么不能离线验 |
| --- | --- |
| `createIOBuffer` 返回的描述符能否被 Family 用于传输 | 要 Family 真的调用它 |
| `provideEndpointIDsForConfiguration` 的 ids 是否被 Family 接受 | 要 Family 真的解析 |
| 访问器在我们固件上返回的值是否正确 | 字节匹配证明**代码相同**，不证明**数据布局相同**；实例大小 312 一致是强证据但非充分 |
| DWC3 上 `handleSetAddressPostStatusPhase` 是否必须写寄存器 | 需要读 `AppleT8011USBXDCI` 或实测 |
| 双 owner 的真实行为 | 要上板 |

**最值得注意的一条**：字节匹配证明「函数代码与我们固件里的完全相同」，**但不证明对象布局相同**。
实例大小 312 与 ios_sagit 一致是强证据（不同版本重排实例大小通常不变），但**没有逐字段验证**。
真机上如果访问器返回荒谬值（alt 数 > 256、pipe 数 > 30），就是布局不一致的信号——
这些边界检查已经在实现里，会返回错误而不是崩溃。

---

## 8. 复现

```bash
# 宿主测试
cc -std=c11 -Wall -Wextra -Werror -o endpoint_host package/tests/alioth_usb_endpoint_host.c
./endpoint_host        # -> all checks passed

# 构建（在构建树里，非 worktree）
cd <tree> && python3 scripts/build-alioth-usb.py
# -> BUILT ... (234 vtable slots, 18 pure overrides verified)

# 固件实测的依据
#   实例大小: kernelcache 里 OSMetaClass ctor 的 `mov w3,#0x138` / `#0x78`
#   访问器字节: 与 ios_sagit drivers/abi-22H374/apple-usb-device-abi.json 逐字节比对
#   arena 静态: QcomXnuUsbDebug.c:37 / :705, osfmk/vm/vm_resident.c:163
```
