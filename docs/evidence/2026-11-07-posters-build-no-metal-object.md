# 三个 poster 进程拿到 GPU 设备后，一个 Metal 对象都没建

2026-11-07 傍晚。`fastboot boot`，不写槽。镜像 `tmp/boot-step2.img`
（sha256 `99035cde2122c5a98d4ff641316eeb95df65607c6d2c785a85e47908f556993f`），
读数 `tmp/console-step1801212.txt`（t=370 以后）。

## 1. 里程碑

插件 `NativeBridgeMetalFrontend.mm` 里每个进程第一次建这些东西时，往它自己的文本行写一次：

| 行 | 含义 |
| --- | --- |
| `buf <KiB> 0` | 第一个 buffer |
| `tex <w> <h>` | 第一张 texture |
| `libok <KiB> 0` | 第一个成功编译的库 |
| `pipe 0 0` | 第一条渲染管线 |

行里只留最后一次写，所以「停在哪一级」就是行上的那一条。

## 2. 读数

```
GPUCLI clients=6 mountfail=1/e00002d5/2 ... askers=7[mediaserverd,backboardd,SpringBoard,
        WidgetRenderer_Defa,CollectionsPoster,PhotosPosterProvide,PosterBoard]

GPUCLIENT WidgetRenderer_Defa   problem pipe 0 0
GPUCLIENT SpringBoard           problem d0/1 k1 b0 0M 0x0 c1
GPUCLIENT CollectionsPoster     entry CollectionsPoster
GPUCLIENT PhotosPosterProvide   entry PhotosPosterProvider
GPUCLIENT PosterBoard           entry PosterBoard

GPUCCLI PosterBoard opens 2 refused 0 closes 1 slot 4 peak 4096 last 0
```

## 3. 两条结论

1. **里程碑是工作的**：WidgetRenderer 走到了 `pipe`。所以 poster 的 `entry` 不是探针失效。
2. **poster 侧一个 Metal 对象都没有**：没有 buffer、没有 texture、没有 library、没有 pipeline。
   它们不是失败在某次 Metal 调用上——是没有调用。

`peak 4096` 要说准：`windowPeak()` 只在一个成功的 map 之后更新，4096 表示最高端点落在
`GpuClientVaStart`（4 GiB）之上不到 1 MiB，也就是只有运行时自己建设备时的那点缓冲。
不是「零字节」，也不是应用资源。

## 4. 边界

壁纸缺席因此不是 Metal 实现的问题；我们的插件在这三个进程身上只做了 `deviceForPort` 给设备。
要再往下走，得量 poster 宿主侧的请求（PosterBoard 有没有让 CollectionsPoster 画），
而不是继续改插件。
