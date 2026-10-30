# USB DT 属性三个值的来源可信度

2026-10-30。离线。对应 `dts/sm8250.dtsi` 里 `usb-device@a600000` 的三个属性。

**这三个值现在都到不了 ADT**（`devicetree.py` 的 `U32` 白名单缺这三个键，
见 `2026-10-30-usb-adt-property-gap.md`）。写在这里是为了把「哪些是事实、哪些是猜」
钉死，等白名单修好后直接用。

## 1. `fifo-depth = <27696>` —— **事实**

来源：`reference/k40-kernel/arch/arm64/boot/dts/vendor/qcom/kona-usb.dtsi:44`

```
qcom,dwc-usb3-msm-tx-fifo-size = <27696>;
```

这是 Qualcomm 自己给 SM8250 主 USB 端口配的 TX FIFO 大小，不是推的。

**注意：Apple 的语义与 Qualcomm 的未必一致。** Apple 侧的名字是 `fifo-depth`，
校验是「不得小于 `kFifoOffsetEnd`」，而 `kFifoOffsetEnd` 的值没能从 kernelcache 里
定出来（它不是字符串，是 `AppleSynopsysOTGDevice.cpp` 里的枚举常量）。
如果真机上 `AppleNubSynopsysOTGDevice` 报

```
"%s::%s fifo-depth IORegistry property was %u, less than kFifoOffsetEnd %d\n"
```

那说明 27696 按 Apple 的量纲偏小，报错行里会同时给出两个数，
**`%d` 就是正确下限，直接按它改**。

## 2. `phy-id = <0>` —— **猜**

Apple 的 `phy-id` 是 arbitrator 内部的 PHY 索引，不是 Qualcomm 的寄存器地址。
本板跑的 PHY 是 USB2 femto PHY（`kona-usb.dtsi` 的 `usb2_phy0: hsphy@88e3000`，
`compatible = "qcom,usb-hsphy-snps-femto"`）；SuperSpeed QMP PHY 在固件树里
`status = "disabled"`，且本板按 high-speed 跑。

**为什么猜 0**：只有一个 PHY 在跑，索引从 0 起是 Apple 单端口设备的常见形态。
**没有证据支持这个具体数字**，只是「唯一在用的 PHY 排第一」的默认。

**真机反推方式**：`AppleNubSynopsysOTGDevice::start` 在创建 phyServiceClient 失败时打

```
%s@%s: %s::%s: Can't create phyServiceClient
```

它读 `phy-id` 后用 `AppleUSBPhyServiceClient::initWithServicesPhyPortAndOptions`，
后者在失败时打

```
AppleUSBPhyServiceClient[%p]::%s: initialized AppleUSBPhyServiceClient %p for phy %p
```

以及 `"Invalid phyID" @%s:%d`。**先看 `"Invalid phyID"` 有没有出现**：
出现说明索引越界，从 0 往上试；不出现但 `Can't create phyServiceClient` 出现，
说明索引有效但 arbitrator 还没发布对应服务——那是 ADT 节点顺序/`usb-complex` 的问题，
不是 `phy-id` 的问题。

## 3. `clock-mask = <0x7>` —— **半推**

依据是 `Sm8250.h` 里我们自己 CDC 驱动的实测常量：

```
#define QUD_GCC_CBCR_OFFSETS {0xf010u, 0xf018u, 0xf01cu, 0xf054u, 0xf058u}
#define QUD_GCC_CBCR_REQUIRED 3u
```

即 high-speed gadget 需要前三个分支（`0xf010` master、`0xf018` sleep、
`0xf01c` mock utmi）；后两个（`0xf054`、`0xf058`）是 SuperSpeed PHY 的辅助时钟，
本板不用，`QUD_GCC_CBCR_REQUIRED = 3` 就是「只要求前三个」的意思。

**`0x7` = bit0|bit1|bit2**，假设「bit n 选第 n 个分支」。**位语义是推断**：
Apple 的 `enableDeviceClock(%p, 0x%08x)` 第二个参数就是这个 mask，
但 mask 到具体时钟的映射表在 arbitrator 内部，没读到。

**真机反推方式**：`AppleEmbeddedUSBArbitrator` 打

```
%06lu.%06u %s::%s: %p with mask 0x%08x
%06lu.%06u %s::%s: enable index %d
%06lu.%06u %s::%s: disable index %d
```

`enable index %d` 会逐条列出它实际启用了哪几个索引。**如果它只列出 0、1、2，
说明 mask 0x7 被正确解析；如果列出 3 或 4，说明位语义反了。**

缺 `clock-mask` 时它直接报

```
"clock-mask needs to be defined in the devicetree" @%s:%d
```

## 4. 一条共同的限制

**上面所有「真机反推方式」都依赖 `IOLog` 能读到。**
但 `IOLog` 不进持久日志（`docs/evidence/2026-10-30-usb-stack-architecture.md` §2），
串口也只在 CDC-ACM 起来之后才有。而 USB 栈实验要关掉 CDC-ACM 才能让 Apple 栈
独占控制器。

**所以这三个值的反推通道与 USB 栈实验本身冲突。** 在关 CDC 之前，必须先把
「kext 的 IOLog 进持久日志」这件事解决——否则关掉 CDC 之后，这三个值错在哪里
永远看不见。

这是本轮发现的一个真实循环依赖，不是理论担忧。
