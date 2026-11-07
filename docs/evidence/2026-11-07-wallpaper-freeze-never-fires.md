# `sheng-wallpaper-freeze=1` 从未冻结：Poster 渲染器一次都没画过 200 万像素

2026-11-07 深夜。`fastboot boot`，不写槽。读数来自 of18b
（`tmp/console-of18b0645181.txt`，`sheng-build-tag=of18`）。

## 1. 用了上游文档里的开关，不是自己造的判据

`NEW/UniXNU/docs/drivers/adreno-metal.md:46` 与
`unixnu.com/zh/porting/gpu` 都写明：

> `sheng-wallpaper-freeze=1` isolates wallpaper rendering. A Poster renderer
> retains its first successfully drawn and stored IOSurface output of at least
> two million pixels, then parks later command buffers before runtime submission.

实现见 `NativeBridgeMetalFrontend.mm:1847-1862`（`freezeWallpaperOn()`：进程名含
`Poster` 且不是 `PosterBoard`）与 `:3029-3036`（只接受 `surface` 非空、
`width*height >= 2000000` 的 store）。

这是上游给这个问题准备的判据：**Poster 渲染器有没有画出过一张 ≥200 万像素的图。**

## 2. 读数：本轮看不到冻结报告，判据无效（自我更正）

本轮命令行带 `sheng-wallpaper-freeze=1`，但**没有 `log-gpu=1`**。冻结事件唯一的
报告渠道是 `GLOG`，而 `GLOG` 需要 `SHENG_LOG_GPU`，即 `log-gpu=1`。

原打算让同一句也走 `native_bridge_metal_problem_hook`（`3d2cb02`），但那一版插件
**没有进镜像**：`build-parity.sh` 用 `--gpu-addon tmp/gpu-official-flat.gpuaddon`，
那是 2026-11-06 20:46 预先打好的一包，里面的插件二进制是 **2026-11-06 03:44** 的。
本对话对 `AdrenoMetal` 的所有改动都不在它里面。

所以 `wallpaper froze` 出现 0 次**只能说明没有报告渠道**，不能说明冻结没有发生。
这条判据要到下一轮带 `log-gpu=1`、或插件能重建之后才成立。

（仓库侧已备好：`3d2cb02` 让冻结同时写进 `[gpu client text]`；
`d1afba0` 让每个进入插件的进程写自己的名字。两者都等插件重建。）

同一轮里 Poster 进程都起来了：

```
GPUCLI ... askers=7[mediaserverd,backboardd,PhotosPosterProvide,SpringBoard,
                    WidgetRenderer_Defa,CollectionsPoster,PosterBoard]
WALLPAPER stage=5 status=0      （覆盖层挂载成功，重复出现）
```

桌面成立：

```
RESULT t=390 own=1 sw=65 data=2 lit=576 ds=1 panel=579 askers=7 -> desktop
GPUFRAME ready 1 fail 0 copied 0 gpu offered 60 done 60 fail 0 abandon 0
PANEL nonblack=579/1000 luma=136
```

## 3. 这条读数的含义

`freezeWallpaperOn()` 的判定在 `NBCommandBuffer -finish`：
命令缓冲**完成**（`MTLCommandBufferStatusCompleted`）、`encodedWork.draws` 非零、
且这一帧里 store 过一张 ≥200 万像素的 surface。三条全满足才冻结。

三轮（of17 / of18 / of18b）里它一次都没满足。所以：

**CollectionsPoster（或任何名字含 Poster 的进程）从未完成过一张铺满屏幕的绘制。**

这排除了"渲染了但没合成"——渲染这一步本身没发生。

## 4. 与之前读数的关系

| 读数 | 来源 | 现在怎么读 |
| --- | --- | --- |
| `[gpu clients]` 有 CollectionsPoster，`s4 ... w0M` | of15b | 打开设备，但写入 0 MiB |
| `prime.log` 零次 CollectionsPoster | of14 | 没编译过任何管线 |
| `[gpu objects] s4 w117M subs=29` | of17 | s4 是**共享槽**（WidgetRenderer/CollectionsPoster/PosterBoard 都在 s4），117 MiB 是 WidgetRenderer 写的 |
| freeze 不触发 | of18/of18b | 没有任何 Poster 进程画出全屏 |

`[gpu clients]` 只有 3 行文本（mediaserverd / SpringBoard / WidgetRenderer），
没有 CollectionsPoster——它没有产生任何插件文本。

## 5. 未完成的部分

`[gpu plugin]` 行（`BootLog.c` 新加，读 `native_adreno_metal_starter`）在这三轮里
都是空的，因为那个列表只在插件标记值 1 时写入，而**标记 1 只由裸用户测试发出，
publish 路径不发**。已确认（`Adreno740/NativeAdrenoPublish.cpp:941-951`）。

`streamText("entry <proc>")` 那处改动（`d1afba0`）已随 gitlink 更新（`5413d07`、
`97c3e65`）进入 of18 的构建，但 of18/of18b 的 `[gpu client text]` 里仍只有三个
进程——这和"CollectionsPoster 不进 `deviceForPort`"一致，但还需要一轮干净的读数确认。

## 6. 下一步

判据不变，用上游的开关：`sheng-wallpaper-freeze=1` 加 `log-gpu=1`，读
GLOG 那一行（现在它同时进 `[gpu client text]`，`3d2cb02`）。

真正要回答的是：PosterBoard 有没有**要求** CollectionsPoster 渲染。上游文档写明
`PosterBoard is the manager, not a renderer`，所以问题在管理侧——它有没有发出渲染请求。
这要读 PosterBoard 自己的日志，而不是 GPU 侧。
