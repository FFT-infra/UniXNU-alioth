# 主屏第一次被看到：图标与 dock 都在，壁纸区域纯黑（of15b）

2026-11-07 深夜。`fastboot boot`，不写槽。读数来自
`build/sheng-logs/of15-000000004d007332/`（`sheng-build-tag=of15`，第二轮
重试，记作 of15b）。

## 1. 桌面成立

```
RESULT t=390 own=1 sw=309 data=2 lit=976 pa=1bea8c000 ds=1 panel=982 frame=308 askers=6 -> desktop
SUPER t=90s  sw=92/92/0  own=1 prep=92
SURFACEL n=4 nonblack=575/1000 luma=134 mx=26/197/251 sp=225
PANEL nonblack=579/1000 luma=135
```

`sw=309` 是这块板目前最高的无人值守帧数（of11 是 231）。

## 2. 导出的截图是主屏

`diag/screen.bmp`（540x1200，半分辨率扫描缓冲）：

- 顶部状态栏 `4:03 PM  Wed Dec 31`
- 中部 App Store 与 Settings 图标（带名字）
- 底部 dock：Messages、Safari、Photos、App Store、Settings 等
- 大块白色区域是应用文件夹背景

**这是主屏，不是锁屏、不是 Settings、不是 boot menu。** 之前的 of8 是 Settings，
of14 是锁屏，两轮都不是主屏；这一轮终于是。

## 3. 壁纸区域是纯黑

按像素统计（`diag/files/screen.bgrx`，BGRX，stride 2160）：

```
整幅 top colors: (255,255,255) 20288  (0,0,0) 16879  (58,58,58) 758
壁纸带（rows 60..180，左半）: (0,0,0) 17216  (255,255,255) 15095
```

`(255,255,255)` 是文件夹背景，`(0,0,0)` 是壁纸位置。**没有一个像素来自壁纸图。**

## 4. overlay 里的图是彩色的

`tmp/wallpaper-official-flat/J120.png`（2732×2732）是一张绿→橙→粉的渐变图。
所以「图本身是黑的」这条排除。

## 5. CollectionsPoster 打开了 GPU 设备，但不提交任何渲染

`build/sheng-logs/of15-000000004d007332/latest.txt`：

```
[gpu clients] ... CollectionsPoster 2/0/1 s4 4096M r0 ...
[metal askers] ... CollectionsPoster:1 ...
[gpu objects] ... s4 16/16 r0 0/0 w0M subs=2 ibs=2
[gpu client text] mediaserverd | SpringBoard | WidgetRenderer_Defa | Preferences
```

四条读数一起看：

- `[gpu clients]` 里**有** `CollectionsPoster`，占 **s4 槽**、映射到 4096M。
  所以它确实打开了 GPU 设备。
- `[metal askers]` 记它读过 `MetalPluginName` 一次。
- `[gpu objects]` 的 s4 槽是 **`w0M`（写入 0 MiB）、`subs=2`**。
  它和 `Preferences` 共用 s4，两者都打开了设备但一个字节都没写。
- `[gpu client text]` 只有四个进程，**没有** `CollectionsPoster`。
  这张表由插件的 `streamText` 写，所以它没有产生任何插件活动文本。

对照 SpringBoard 的 s0：`w482M subs=623 ibs=5907`——GPU 写入完全可用。

**所以断点不是「没调用插件」，而是「调用了、拿到设备、但没提交渲染工作」。**

（更正：本轮镜像另加的 `0x6903` 标记没有出现在任何一行。`marks[16]` 是环，
mediaserverd 的 `6730/6731` 每帧填满它，开机时发一次的值早已被覆盖。
控制台里那个 `6903` 是 `GPUMETH ... us 6903` 的**耗时**，不是标记。
改用 `streamText` 写进程自己的文本行，已在 `d1afba0` 提交，待验证。）

## 6. 这一轮排除了什么

| 假设 | 判据 | 结论 |
| --- | --- | --- |
| 桌面进不来 | `sw=309 own=1`、截图是主屏 | 排除 |
| 壁纸图是黑的 | `J120.png` 是彩色渐变 | 排除 |
| 显示链写错 | `SURFACEL` 与 `PANEL` 亮度一致（134/135） | 排除 |
| GPU 不工作 | `ZAP ok`、`METAL ok`、`offered/done` 匹配、`askers=5` | 排除 |
| CollectionsPoster 调用了插件但失败 | `marks` 里无 `6903` | 排除 |
| **CollectionsPoster 读了属性后没调插件** | `askers` 有、`marks` 无 `6903` | **成立** |

## 7. 下一步

要回答的是：`MetalPluginName` 读到之后，到 `deviceForPort` 之间发生了什么。

可查的位置：

1. `native_adreno_note_property_read` 记下的是**读属性那一刻**的 `proc_selfname`。
   如果读属性的进程是宿主（PosterBoard / backboardd）而不是扩展本身，
   `askers` 里出现 `CollectionsPoster` 可能是进程名继承的结果。
2. PosterKit 的 `wallpaperPosterRenderController` 是实际渲染入口
   （`CollectionsPoster` 二进制里没有 `MTLCreateSystemDefaultDevice` 一类调用，
   它链接的是 `QuartzCore` / `PosterKit` / `WallpaperKit`）。
3. 锁屏与主屏用的是同一个 `.ca`（`Wallpaper.plist` 的 `assets = {lockAndHome}`），
   所以两处黑是同一个原因。
