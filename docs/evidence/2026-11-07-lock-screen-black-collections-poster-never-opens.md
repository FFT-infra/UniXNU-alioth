# 第一次看到锁屏：纯黑；CollectionsPoster 读了 MetalPluginName 但从不打开设备

2026-11-07 深夜。`fastboot boot`，不写槽。读数来自 of14 的持久日志与诊断导出。

## 1. 这是第一次看到壁纸应该出现的界面

`of14` 的 `screen.bgrx`（`build/sheng-logs/of14-000000003c76fbc3/diag/screen.bmp`）：

- 时间 `4:03`、日期 `Wednesday, December 31`、右上角电池、底部 home indicator
- **背景纯黑**，壁纸位置没有任何图像

之前的 of8 是 Settings（已在
`2026-11-07-of8-was-settings-not-home-screen.md` 更正），另两轮停在 boot menu。
这是第一轮停在**锁屏**——而锁屏正是 `CollectionsPoster` 声明的
`PRPosterRoleLockScreen`。所以这一轮的观察对象终于是对的。

## 2. GPU 本身工作正常

```
GPUROW21 ZAP ok step 7 init 0/0 mem 0/0 auth 0/0 secure 0
GPUROW24 METAL ok: accelerator registered (IOGPUFamily loaded, class size 136)
GPUFRAME ready 1 fail 0 copied 0 gpu offered 9 done 9 fail 0 abandon 0
GPUSWAP validate 9 ok 1 reason 0 | copied 9 status 0 bytes 10368000 | retired 9
GPUHANG h=0 r=0 f=0 fence=0
[gpu objects] s0 164/171 w244M subs=31 ibs=205   ← SpringBoard 写了 244 MiB
```

SpringBoard 的 s0 槽写了 244 MiB，说明 GPU 写入路径完全可用。

## 3. 壁纸进程的槽是空的

```
[gpu clients] kernel_task s- | mediaserverd s1 | backboardd s0 | SpringBoard s2
              WidgetRenderer_Defa s3 | PhotosPosterProvide s4 | PosterBoard s4
[gpu objects] s0 164/171 w244M subs=31 ibs=205 | s1 9/18 w20M subs=4 |
              s2 21/21 w58M subs=3 | s3 30/30 w45M subs=7 | s4 16/16 w0M subs=2
```

`s4` 是 PosterBoard / PhotosPosterProvide 的槽：**`w0M`（写入 0 MiB）**，
只提交 2 笔。它们分配了 16 个对象，但一个字节都没写进 GPU 地址空间。

## 4. 更精确的一步：CollectionsPoster 读了属性，但从不调用插件

```
GPUCLI clients=1 mountfail=1/e00002d5/2 ... askers=5[
  mediaserverd,backboardd,SpringBoard,WidgetRenderer_Defa,CollectionsPoster]
GPUCLIENT mediaserverd:  ...
GPUCLIENT SpringBoard:   ...
GPUCLIENT WidgetRenderer_Defa: ...
```

- `askers` 里**有** `CollectionsPoster`：它读了 `MetalPluginName` 属性
  （`NativeAdrenoPublish.cpp:1039-1049` 的 `native_adreno_note_property_read`）。
- `GPUCLIENT` 只有 3 个进程，**没有** `CollectionsPoster`。
- `prime.log` 里 `CollectionsPoster` 出现 **0 次**——它没编译过任何 Metal 管线。

读属性与调用插件是两件事。插件入口
（`NativeAdrenoMetalPlugin.mm:258`）：

```c
if (strcmp(getprogname(), "backboardd") != 0) return device = clientDevice(port);
```

只有 backboardd 走发现路径并留 `0x6800` 标记，其他进程走 `clientDevice`。
`clientDevice` 无论成功还是失败都会留痕迹：成功进 `[gpu clients]`，
失败走 `clientFailure()` 打 `GPUCLIENT` 文本行。**CollectionsPoster 两者都没有。**

所以它停在「读了 `MetalPluginName`」和「调用插件」之间。

## 5. 另有一条独立的失败

```
mountfail=1/e00002d5/2
```

`0xe00002d5` = sys 0x2d5 = **725** = `kIOReturnNotPrivileged`。
一次 GPU 窗口挂载被特权拒绝。这条属于 GPU 侧，尚未确定是否与壁纸有关，
但它是这一轮唯一一个非零的错误码。

## 6. 统一日志确实在跑

`logdata.statistics.0.txt` 记 `total : 8482217`，`logd.0.log` 227 KiB。
`Special/*.tracev3` 里有 2 MB 的文件。macOS 的 `log` 工具拒绝这个归档
（版本 7，来自 iPadOS 17，`log: The log archive is corrupt or incomplete`），
所以只能用 `strings` 提取，精度有限。

从 tracev3 提取到的壁纸相关字符串：

```
xpcservice<com.apple.WallpaperKit.CollectionsPoster(...)>
target:[xpcservice<com.apple.WallpaperKit.CollectionsPoster(...)>]
com.apple.MobileAsset.Wallpaper
com.apple.posterkit.provider
```

`26-27-*` 是 Messages 进程引用 CollectionsPoster 作为壁纸提供者。
**扩展是被系统认到并调用的**，不是找不到。

`couldn't obtain URL** extensionNotFound(...)` 属于 `com.apple.tips.TipsSwift`
（Tips 小组件），与壁纸无关，不要误读。

## 7. 下一步的判据

下一步要回答的是：CollectionsPoster 读了 `MetalPluginName` 之后，为什么没调插件。

可用的读数：

1. 在插件入口加一条「进入」标记，写在 `getprogname()` 分支之前，
   这样任何进程调用插件都会留痕。现在只有 backboardd 会留。
2. 在 `native_adreno_note_property_read` 里记下读属性之后该进程的后续动作
   （是否调了 `newUserClient`）。
3. `mountfail` 的 `kIOReturnNotPrivileged` 要单独查：哪一次挂载、谁发起的。

判据：`GPUCLIENT` 里出现 `CollectionsPoster` 的行，或 `prime.log` 里出现它。
