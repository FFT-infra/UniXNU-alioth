# 壁纸黑的直接原因：Poster 侧进程一个都没画过

2026-11-07 深夜，`fastboot boot`，不写槽。镜像 `tmp/boot-render3.img`
（sha256 `2c80b2c9a5e8f28fb3e9a800532353edc442d2dcaf878a235a60d0ba7b05c76f`）。
读数来自 `tmp/console-rd30755391.txt` 与持久日志 `build/sheng-logs/alioth-0000000026d3437c`
（`boot_id 0x26d3437c`，`uptime_s 397`）。

## 1. 判据：每个 Metal 进程自己报画了什么

上一轮（of18b）用上游的 `sheng-wallpaper-freeze=1` 判「Poster 渲染器有没有画出过
≥200 万像素」，读数是不冻结。那条判据只能证明「没画满屏」，不能分开两种情况：
**完全没画**，还是**画了但小**。

为此在 `NativeBridgeMetalFrontend.mm` 的 `-finish` 里加了一行，写进该进程自己的
文本行（`native_adreno_client_text`，驱动每个进程一行，共 8 行）：

```
d<有绘制的命令缓冲>/<完成的命令缓冲> k<dispatch> b<blit> <像素>M <最大存储 surface> c<提交数>
```

只在进程还没有报过真实问题时写，所以客户端的失败文本仍占得住自己那一行
（`9ffd5e7`）。这一行不进命令行，因为命令行是 1006/1023 字节。

## 2. 读数

`[gpu client text]`（同一份日志重复出现，内容一致）：

```
mediaserverd:         problem1 Values:pipelineLibrary:error: (answered nil)
SpringBoard:          d0/1 k1 b0 0M 0x0
WidgetRenderer_Defa:  d4/4 k0 b0 1M 800x800
PosterBoard:          d0/1 k1 b0 0M 0x0
CollectionsPoster:    entry CollectionsPoster
PhotosPosterProvide:  entry PhotosPosterProvider
GradientPosterExten:  no-device device: Vulkan(VkResultCode(-3))
EmojiPosterExtensio:  no-device device: Vulkan(VkResultCode(-3))
```

`[metal askers]`：9 个进程要过 Metal —
`mediaserverd backboardd SpringBoard WidgetRenderer_Defa CollectionsPoster
PhotosPosterProvide PosterBoard GradientPosterExten EmojiPosterExtensio`。

`[gpu clients]`：

```
PosterBoard 2/0/1 s4 4240M | PhotosPosterProvide 2/0/1 s4 4096M |
CollectionsPoster 2/0/1 s4 4096M | GradientPosterExten 1/0/1 s- 0M |
EmojiPosterExtensio 1/0/1 s- 0M
```

`[gpu objects] ... s4 16/28 r0 0/0 w0M subs=9 ibs=18` —— s4（PosterBoard /
CollectionsPoster / PhotosPosterProvide 共用）写入 **0 MiB**。

## 3. 三条结论

1. **CollectionsPoster 与 PhotosPosterProvider 拿到了设备，但没有完成过任何一条命令缓冲。**
   它们的行停在 `entry <name>`，而那行只由 `deviceForPort` 写；`-finish` 一次都没走。
   这一条同时排除了「画了但小」——它们没有画。
2. **PosterBoard 与 SpringBoard 各完成一条命令缓冲，里面只有 1 个 dispatch、0 个 draw、
   没存过任何 surface。** 这看着是着色器编译/预热那一下，不是合成。
3. **GradientPosterExtension 与 EmojiPosterExtension 连设备都没拿到**：
   `Vulkan(VkResultCode(-3))`，即 `VK_ERROR_INITIALIZATION_FAILED`。

只有 WidgetRenderer_Defa 在正常画（4/4，最大 800x800，图标/小组件尺寸），
`backboardd` 那条在 `[gpu clients]` 里是 s0 且 subs=41，屏幕本身是它在合成。

所以黑壁纸不是合成问题，也不是像素格式问题：**壁纸渲染器没有产出任何一张图。**

## 4. 这一轮没能分开的两件事

行停在 `entry` 有两种可能，这一行分不开：

* CollectionsPoster 根本没编码/提交任何东西（用户态没走到渲染）；
* 它提交了，但命令缓冲永远不完成（我们这边的问题）。

下一轮已加两条更低的梯级（`tmp/boot-render4.img`）：

* `newCommandQueue` 第一次成功写 `queue 1`，失败写 `queue refused <原因>`——
  队列被拒的进程会静默停在 `entry`，和「没有东西可画」读数完全一样；
* 第一次 `commit` 写 `cmt 1 d<draw> k<dispatch> b<blit> p<pass>`，`-finish` 那行加上 `c<提交数>`。

这样 `entry → queue → cmt → d…` 四级能把「没走到」和「提交了不完成」分开。

## 5. 与旧读数的关系

| 旧读数 | 现在怎么读 |
| --- | --- |
| of18b 用 `sheng-wallpaper-freeze=1` 不冻结 | 现在有直接证据：没有任何 Poster 进程存过 surface，谈不上「≥200 万像素」 |
| of17 `[gpu objects] s4 w117M subs=29` | s4 是共享槽，那 117 MiB 是 WidgetRenderer 的；本轮同一槽 `w0M` |
| `[gpu clients]` 里 CollectionsPoster `w0M`（of15b） | 与本轮一致，且现在知道它连一条命令缓冲都没完成 |
