# iOS USB 设备栈：usbmuxd 在主机侧，不在设备侧

2026-10-30，alioth（SM8250-AC）。只读分析 + 一次离线 prelink 实验。**没有真机轮次。**

- 设备仓库：`FFT-infra/UniXNU-alioth` @ `ebf998faf5a84be16110ac152d800e4bd2a8e478`
- 基仓库：`NEW/UniXNU` @ `c1b38a817a737921224659911261059804e9e88f`
- kernelcache：`out/ipsw/21H461/kernelcache.decompressed`，sha256 `f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7`
- 根卷：IPSW 内 `044-76040-038.dmg`（OS，5157406069 B，sha256 `3f99b859…`）与
  `044-76034-038.dmg`（SystemOS cryptex，3303014400 B，sha256 `b4908cde…`），
  经 `hdiutil attach -readonly` 只读挂载，未解包、未改写。

本文件更正 `reference/IOS-USB-STACK-ANALYSIS.md` 的 §4.4 与 §5，并给出可复核的替代读数。

---

## 1. 【做了什么】

1. 只读挂载两个 IPSW 卷，枚举 `/usr/libexec`（293 个条目）、`/usr/bin`、`/usr/sbin`、
   `/bin`、`/sbin`、`/System/Library/LaunchDaemons`（523 个 plist），并按文件名与内容搜
   `usbmux` / `mux`。
2. 用 `plistlib` 解析 lockdownd / installd / afcd / appstored 的 plist。
3. 用 `kits/XnuPrelink/native_prelink.py` 的 `unserialize_xml()` 解析 kernelcache 的
   `__PRELINK_INFO`，读出 personalities 的真实值（IDREF 已解引用，不是 XML 里的字面量）。
4. 用 `emit_prelink.py` 跑一次**离线** prelink：原有 31 条 roots 加 4 条 USB roots。
5. 读 `build/sheng-logs/u266-00000007408077f8/latest.txt` 的 `[spawn]` census。

## 2. 【关键证据】

### 2.1 usbmuxd 不在设备上，但**本来就不该在**

| 检查 | 结果 |
| --- | --- |
| 两卷内任意 `*mux*` 文件名（`find -iname`） | **0** |
| `/usr/libexec` 里含 `mux`/`usb` 的名字 | **0** |
| 523 个 LaunchDaemon plist 中提到 `usbmux` | **0** |
| 523 个 LaunchDaemon plist 中提到 `62078` | **0** |
| dyld 共享缓存（54 个分片）里 `usbmux` | **1 条**，在 `.41` 分片，是客户端串 `SocketStreamHandlerConnect: Connection via direct usbmux is deprecated` |

**`usbmuxd` 是主机侧守护进程。** macOS 上它就在跑：

```
srw-rw-rw-  /var/run/usbmuxd
_usbmuxd  426  /System/Library/PrivateFrameworks/MobileDevice.framework/Versions/A/Resources/usbmuxd -launchd
```

把它当成「设备上缺的零件」是路线判断错误，不是缺件。§4.4 的枚举本身没错（376 个
`/usr/libexec` 条目里确实没有），但由它推出的「整条路被否决」不成立。

### 2.2 内核里的 `AppleUSBDeviceMux` 是**客户端**，不是服务端

从 kernelcache 的字符串表（`0x105b000..0x105d000`）读出它的实现：

| 证据 | 值 |
| --- | --- |
| `sock_connect` | **2** 处 |
| `sock_receive` | 1 处 |
| `sock_bind` / `sock_listen` / `sock_accept` | **0 / 0 / 0** |
| `%s new session to port %d failed: %d`（`handleConnectResult`） | 存在 |
| `%s The usbmuxd attempted to set the log level …`（`handleMuxHostLogLevelInput`） | 存在 |
| `USB mux: %lu reads / %lu errors, %lu writes / %lu errors`（`reportStats`） | 存在 |

也就是说：**内核 mux 从 USB 收到 mux 报文，然后把每一路连接 `sock_connect` 到本机某个
TCP 端口。** 它消费的是「host 发来的 mux 请求」，不是「设备发起的请求」。

与之对接的设备侧进程是 **`lockdownd`**，证据如下：

| 证据 | 位置 |
| --- | --- |
| `AppleUSBDeviceMux`、`IOUSBDeviceInterface`、`AppleUSBMux`、`mux_notification_handler` | `lockdownd` 的字符串表 |
| `Failed to create IOKit matching dictionary for 'AppleUSBDeviceMux'.` | 同上 |
| `Misc usbmuxd event: (messageType=0x%08x, argument=%p)` | 同上 |
| `ioKitTransport`（唯一的 transport 名） | 同上 |
| LaunchDaemon plist 的 `Sockets`：IPv4/IPv6 `SockServiceName = 62078`，Unix `/var/run/lockdown.sock` | `com.apple.mobile.lockdown.plist` |
| LaunchDaemon plist 的 `LaunchEvents`：`IOPortTransportState` + `DataRole=1` + `TransportType=2/3`（USB2/USB3 device） | 同上 |

**62078 是 lockdown 端口。** lockdownd 在设备上**监听** 62078，这一条是事实（plist 的
`SockServiceName`）。内核 mux 把它收到的连接请求 `sock_connect` 到本机某个端口，
端口号来自 host 报文——这一条也是事实（`%s new session to port %d failed` 的 `%d`）。

**推断（有依据、未验证）：** 那个 `%d` 由 host 的 mux 报文指定，host 要连 lockdownd 时
填 62078。依据是 usbmux 协议的分帧设计（mux 报文携带目标端口，内核不自己选端口）与
`AppleUSBDeviceMux` 的 `handleMuxInput` 路径。**边界：** 没有抓到实际报文，
也没有在板上跑过，所以「62078 被填进去」是推断而不是读数。

（旁证：`AppleUSBDeviceMux` 的 `OSBundleLibraries` 里没有 lockdownd，因为 lockdownd 是
用户态；内核 mux 只用 `com.apple.kpi.bsd` 的 socket 调用。）

### 2.3 目标 kext 的 personality 真实值（IDREF 已解引用）

`reference/IOS-USB-STACK-ANALYSIS.md` §3.5 引用的 plist 是 XML 字面量，其中
`IOProbeScore` 与 `IOPlatformPanicAction` 是 `IDREF`，不是字面整数。真实值：

```
com.apple.driver.AppleSynopsysOTGDevice
  IOClass              AppleNubSynopsysOTGDevice
  IOProviderClass      AppleEmbeddedUSBNub
  IONameMatch          usb-device,t7000
  IOProbeScore         10000          ← 不是 10001
  IOPlatformPanicAction 1000          ← 不是 "set"
```

`IOPlatformPanicAction` 是**panic 时回调的优先级**（`kIOPlatformPanicActionKey`，
`IOKitKeysPrivate.h:96` 注释为 `value is OSNumber (priority)`；消费者是
`IOCPURunPlatformPanicActions()`，在 `IOPlatformExpert.cpp:1059-1091` 由 panic 路径调用）。
**它不是「匹配失败就 panic」的开关。** 所以 §3.5 与 §7「不匹配会 panic」的表述要改：
不匹配的后果是驱动不 probe，USB 栈静默缺失，不是 panic。

### 2.4 ADT 节点不能靠 `[adt.*]` 表加进去

`scripts/unixnu/devicetree.py:379` 只对固定名单生成 platform 节点：

```python
for name in ('defaults', 'options', 'product', 'buttons'):
```

`arm-io` 的子节点只来自 DTS 里带 `unixnu,adt-name` 的节点（`:371-378`）。所以
`[adt."usb-complex"]` 写进 `board.toml` 会被**静默丢弃**，必须改 DTS。

而且 `IONameMatch` 用的是 `IORegistryEntry::compareName()`
（`IORegistryEntry.cpp:1173-1190`），是 `copyName()` 的**精确相等**，不查 `compatible`。
所以 ADT 节点名必须**逐字符等于** `usb-complex,s5l8900x` 或 `usb-device,t7000`，
不能靠 `compatible` 列表匹配。

### 2.5 离线 prelink 实验：四条 USB root 全部链接成功

在 `tmp/usbstep4/` 用 `emit_prelink.py` 跑了一次，输入与产线构建完全相同
（同一个 `mach.development.bcm2837.unstripped.noctf`、同一个 kernelcache、同一个 slide
与 `early_security_init`），只把 roots 从 31 条加到 35 条：

| 项 | 产线 kernel | USB 实验 kernel |
| --- | --- | --- |
| components | 30 | **42** |
| 输出字节 | 125583360 | 125808640 |
| sha256 | — | `bb22c8ff5e3a4ce22d712b4e3e7008632f5948018172aa27984f12f5a78533b6` |
| `exit` | — | **0** |

新增的 12 个组件：

```
com.apple.driver.AppleEmbeddedUSB
com.apple.driver.AppleOnboardSerial
com.apple.driver.AppleSynopsysOTGDevice      ← DWC3 设备侧驱动
com.apple.driver.AppleUSBDeviceMux
com.apple.driver.mDNSOffloadUserClient-Embedded
com.apple.driver.usb.AppleUSBCommon
com.apple.iokit.IOAccessoryManager
com.apple.iokit.IONetworkingFamily
com.apple.iokit.IOPortFamily
com.apple.iokit.IOSerialFamily
com.apple.iokit.IOSkywalkFamily
com.apple.iokit.IOUSBDeviceFamily
```

产物里逐条核对：

| 符号 | 命中 |
| --- | --- |
| `IOUSBDeviceFamily` | 3 |
| `AppleUSBDeviceMux` | 11 |
| `AppleNubSynopsysOTGDevice` | **2** |
| `AppleEmbeddedUSBArbitrator` | 4 |
| `IOPortTransportState` | 13 |

`__PRELINK_INFO` 里 4 条 USB bundle 的 personality 都在（共 50 条条目）。
`abi_qualified=False`、`loadable=False` 与产线一致——这是 prelink 工具对「未做运行期
资格验证」的如实标记，不是本次新增的失败。

**这一步证明的是「能链接」，不是「能跑」。** 寄存器、PHY、DT 属性全部未验证。

### 2.6 `[kernel.prelink] roots` 是整表覆盖，不是合并

`scripts/unixnu/profiles.py:9-16` 的 `merge()` 对非 dict 值直接覆盖。实测：

```
base roots ['r1','r2','r3'] + board roots ['r1','r2','r3','NEW'] -> ['r1','r2','r3','NEW']
```

所以在 `board.toml` 写 `[kernel.prelink] roots` 必须**重复全部 31 条**再加新条目，
否则其余组件全部丢失。`profiles/` 属于上游，按约束不动；这条只能落在设备仓库。

### 2.7 板上一轮的真实进程数是 233，不是 36

`build/sheng-logs/u266-00000007408077f8/latest.txt` 的 `[spawn]` census（`BootLog.c:1038`
起，来自 `BootProgress.c` 的 `census_note()`，由 `kern_exec.c` 的
`exec process signature` 路径记录**启动**）：

```
names=233 dropped=2
```

其中相关项（`started/exited@首次秒`）：

```
containermanagerd 2/0@26    misd 2/0@26      appstored 2/0@39
containermanagerd_syste 2/0@27   atc 2/0@30   misagent 2/0@44
lockdownd 2/0@30    backboardd 1/0@30   SpringBoard 2/0@26
xpcproxy 399/203@26
```

**`installd` 不在其中，`afcd` 也不在。** 但 233 这个数字说明「只起了 36 个进程」这个
读法是错的：36 来自 `EXITLOG` 的**去重进程名数**（`BootProgress.c:983` 的
`boot_progress_exit()` 只在进程**退出**时记录），退出过的进程才有行，长期运行的守护
进程永远不会出现在 EXITLOG 里。`EXITLOG` 不能用来数「起了哪些进程」。

## 3. 【下一步】

1. 判据已改：`usbmuxd` 不是阻塞项，真正的阻塞是 (a) 内核缺 USB kext、(b) 板子没有
   USB 的 ADT 节点、(c) `installd` 没被启动。
2. `installd` 的 plist 没有 `RunAtLoad`，只有 `MachServices`，所以它是**按需**启动的；
   要判断它为什么没起，得看谁应该向 `com.apple.mobile.installd` 发起 lookup，以及那个
   lookup 有没有失败。u266 的 `[spawn]` 没有 installd，但也没有 lookup 失败记录
   （内核侧不记录 launchd 的 lookup）。这条需要用户态日志（`logd`/os_log），当前没有通道。
3. USB 路线的最小实验见 `decisions/2026-10-30-usb-stack-route.md`。
