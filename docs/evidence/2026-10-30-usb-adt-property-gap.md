# USB ADT 节点：属性白名单会静默丢弃 `fifo-depth` / `phy-id` / `clock-mask`

2026-10-30。**离线静态验证，无真机。** 设备仓库 `FFT-infra/UniXNU-alioth` @
`ebf998faf5a84be16110ac152d800e4bd2a8e478`；SoC 仓库改动见 §4。

这是 USB 路线第 3 步（加 ADT 节点）在做之前就撞到的一个阻塞。**它会让「节点加上了、
属性没带上」，而日志里什么都不会说。**

## 1. 【做了什么】

1. 在 `NEW/UniXNU-sm8250-common/dts/sm8250.dtsi` 加了两个节点，带
   `unixnu,adt-name` 与三个 Apple 驱动要求的属性。
2. 用 `dtc` 编译，确认语法与产物正确。
3. 用 `unixnu.devicetree` 的 `read_fdt` / `hardware_properties` / `arm_io_window`
   走一遍生成路径，检查最终 ADT 节点里有什么。

## 2. 【关键证据】

### 2.1 节点本身是对的

编译后的 DTB 里有这两个节点，`adt-name` 与 `reg` 都正确：

```
usb@a600000          -> usb-complex,s5l8900x   reg <0 0xa600000 0 0x100000>
usb-device@a600000   -> usb-device,t7000       reg <0 0xa600000 0 0xd93c>
```

`status = "disabled"` 时 `read_fdt(...).enabled()` 返回 **False**，
`devicetree.py:371` 的 `and entry.enabled()` 把它们排除在 `arm-io` 之外——
**这一点是好的**：节点可以先写进去而不激活。

翻成 `okay` 后，`arm-io` 的子节点名单变成：

```
['intc', 'ufshc', 'ufsphy', 'usb-complex,s5l8900x', 'usb-device,t7000']
```

**名字对了。** 这是路线 A 的第一步。

### 2.2 但三个属性被静默丢弃

`hardware_properties()` 只拷贝**白名单内**的属性键：

```python
STRINGS    = {'compatible','device_type','reg-names','interrupt-names','clock-names','method'}
U32        = {'phandle','linux,phandle','#interrupt-cells','#iommu-cells','#clock-cells',
              'clock-frequency','timebase-frequency'}
U64        = {'redistributor-stride'}
FLAGS      = {'interrupt-controller','dma-coherent'}
REFERENCES = {'interrupt-parent':None,'iommus':'#iommu-cells','clocks':'#clock-cells'}
```

`fifo-depth`、`phy-id`、`clock-mask` **一个都不在**这五个集合里
（`grep -c 'fifo-depth\|phy-id\|clock-mask' devicetree.py` = **0**）。

实测：喂一个带这三个属性的节点进去，

```python
properties = {'fifo-depth': b'\x30\x6c\x00\x00', 'phy-id': b'\x00\x00\x00\x00',
              'compatible': b'snps,dwc3\x00'}
hardware_properties(entry, {}, 0)
# -> [('compatible', b'snps,dwc3\x00')]
```

**只剩 `compatible`。另外两个消失了，且没有抛错。**

`hardware_properties()` 的 if/elif 链没有 `else` 分支，未知键既不追加也不报错——
这正是最坏的形态：**配置写对了、产物里没有、日志里不说**。

### 2.3 后果

`AppleNubSynopsysOTGDevice` 的三个必需属性全部读不到，它会按自己的字符串表报错：

| 缺的属性 | 驱动会打印 |
| --- | --- |
| `clock-mask` | `"clock-mask needs to be defined in the devicetree"` |
| `fifo-depth` | `"%s::%s fifo-depth not provided in device-tree\n"` |
| `phy-id` | 无专门报错，走 `Can't create phyServiceClient` |

**但这三条是 `IOLog`，而 `IOLog` 不进持久日志**（见
`docs/evidence/2026-10-30-usb-stack-architecture.md` §2 与 u272 那一轮）。
所以真机上会表现为「节点加了、USB 还是没起来、日志里什么都没有」。

## 3. 【下一步】两条修法，都要动上游或加板级 guard

| 修法 | 做什么 | 代价 |
| --- | --- | --- |
| A. 上游加白名单 | 在 `NEW/UniXNU/scripts/unixnu/devicetree.py` 的 `U32` 里加这三个键 | **改上游通用文件**，按约束必须先问用户 |
| B. 板级预检 | 在设备仓库加一个检查脚本，构建前断言「凡 `unixnu,adt-name` 节点上的属性键都在白名单内」 | 不改上游；把静默失败变成硬失败 |

**B 已实现**：`NEW/UniXNU-alioth/scripts/check-adt-properties.py`。
它走真实的 `read_fdt()` + `hardware_properties()` 路径，断言凡导出的 ADT 节点上的
属性键都在写入白名单内。实测两种输入：

```
当前 DTS（USB 节点 disabled）      -> exit 0  "every exported ADT node carries only writable properties"
USB 节点翻成 okay                  -> exit 1  'ADT node "usb-device,t7000" drops: clock-mask, fifo-depth, phy-id'
```

**建议 A 才是真正的解法**（零上游改动、立刻消除盲区只是 B 的一半），
A 要改 `NEW/UniXNU/scripts/unixnu/devicetree.py`，按约束必须先问用户。

**注意 B 只能「报错」，不能「带上属性」**：属性仍然进不了 ADT。
所以即使 B 就位，路线 A 在 `fifo-depth` 这类属性上**仍然不通**，除非做 A。
这一点必须说清，否则 B 会给人「问题解决了」的错觉。

## 4. 本轮对 SoC 仓库的改动

`NEW/UniXNU-sm8250-common/dts/sm8250.dtsi` 新增两个节点（`status = "disabled"`）：

| 节点 | `unixnu,adt-name` | 属性来源 |
| --- | --- | --- |
| `usb@a600000` | `usb-complex,s5l8900x` | reg 从 `kona-usb.dtsi` 的 `ssusb@a600000` |
| `usb-device@a600000` | `usb-device,t7000` | reg 从同文件的 `dwc3@a600000`（`0xd93c`） |

`fifo-depth = <27696>` 取自 vendor DTS 的
`qcom,dwc-usb3-msm-tx-fifo-size = <27696>`（**这是事实，不是猜的**）。

`phy-id = <0>` 与 `clock-mask = <0x7>` 是**推断**：

- `phy-id`：Apple 的 PHY 索引，不是 Qualcomm 寄存器；本板跑 USB2 femto PHY
  （`usb2_phy0: hsphy@88e3000`），SS QMP PHY 在固件树里是 disabled。索引 0 是猜的。
- `clock-mask = <0x7>`：取 GCC 的 USB30_PRIM 三个分支（`0xf010` master、
  `0xf018` sleep、`0xf01c` mock utmi），与 `Sm8250.h` 的
  `QUD_GCC_CBCR_OFFSETS` / `QUD_GCC_CBCR_REQUIRED = 3` 一致。位掩码的**位语义**
  是推断（假设 bit n 选第 n 个分支）。

**三个属性目前都到不了 ADT（§2.2），所以这些取值现在不起作用。**
写进 DTS 是为了把事实固定下来，等白名单修好后直接生效。
