# 合成触摸驱动的桌面：GPU 交接与导出都成立，壁纸仍缺席（of8）

2026-11-07。设备仓库 `NEW/UniXNU-alioth`。真机读数，`fastboot boot`，不写槽。

## 1. 本轮配置

镜像 `tmp/boot-of8.img`，sha256
`1aebf834543cf0fe1e9775f8f8b525b24d8ec28a5cbdea62a394fdd5997f1806`，`LINK_OK`。
命令行 1004 字节，含 `pipa-gpu=12 pipa-gpu-zap=1 sheng-fb-gpu=1 sheng-gpu-wc=0
sheng-gpu-trace=1 sheng-diag-export=150 sheng-touch-synth=4000`。
addon `gpu-official-flat.gpuaddon` `d484c4fa74c2cf2e`。

本地提交：`7686c72`（合成触摸）、`596983f`（合成触摸配置行）、
`e92cbd7`（持久日志计数器回滚）。

## 2. 合成触摸解决了"空转不出帧"

前一轮的对照已经把原因定死：合成器只在有输入时画。凡是出现可见桌面的轮次都有真手指
（of1 9 次、of1b 27、of3 21、g1 8）；三轮无人触屏的都停在近黑，of6 的 `prep=12 sw=12`
严格一比一。`reference.md:3350` 也记过 `native-touch=0` 时 `prep=11 sw=11`。

本轮 `sheng-touch-synth=4000` 在 `sheng-touch-synth-at=60000` 注入一次 4 秒接触，
走驱动自己的 `publish()`：

```
TOUCHSYNTH cfg hold=4000 at=60000
TOUCHSYNTH down
TOUCHSYNTH up
```

结果：

```
SUPER t=70s  sw=78/78/0  own=1  prep=78
SUPER t=80s  sw=129/129/0 own=1 prep=129
GPUFRAME ready 1 fail 0 copied 0 gpu offered 129 done 129 fail 0 abandon 0
```

**第一次在无人值守下达到可见桌面**（`own=1`、`sw=149`、`RESULT ... -> desktop`）。
触摸释放后帧数停在 129 附近，再次印证每一帧都由输入驱动。

## 3. GPU 零拷贝与导出都成立

```
GPUROW ZAP ok step 7 ... secure 0
GPUROW24 METAL ok: accelerator registered (IOGPUFamily loaded, class size 136)
GPUFRAME gpu offered 129 done 129
GPUSWAP copied 129 retired 129
PANEL nonblack=982/1000 luma=237
GPUFB  nonblack=982/1000 luma=237 sided 18/1000
SURFACEL n=15 nonblack=989/1000 luma=237 mx=0/122/255 sp=255
```

`SURFACEL mx=0/122/255 sp=255` 是关键：合成器表面里有饱和的 iOS 系统蓝，不再是 of6 的
`mx=0/0/0`。所以显示链把真实内容送到了扫描缓冲。

持久日志也第一次读到了本轮自己的头：

```
read-sheng-log: build/sheng-logs/of8-0000000276faef5f
  boot 0000000276faef5f  uptime 397s  rounds 167  slots 167/507  errors 0
  diag 515 files, screen .../diag/screen.bmp
```

这验证了 `e92cbd7` 的修法：计数器在写头之前推进，busy 路径回滚。of6 那次报
`round=113` 而块 0 仍是 `usb190 / slots 509` 的形态没有再现。

## 4. 壁纸仍然缺席，这次有像素证据

导出的 `screen.bgrx`（540x1200，BGRX）直接统计：

```
top colors: (255,255,255) 54.7%   (242,242,247) 37.8%
            (0,0,0) 1.9%  (197,197,199) 0.8%  (0,122,255) 0.8%
wallpaper region (rows 60..180, left half): 全部 (242,242,247)
```

`(242,242,247)` 是 iOS 的系统灰，`(255,255,255)` 是白色页面。壁纸区域是**均匀纯色**，
没有任何图像内容。149 种颜色全部来自图标、文字和控件（含 `255,59,48` 红、
`255,149,0` 橙、`0,122,255` 蓝等系统色）。

结论：**桌面合成正常，壁纸层没有画进合成结果**。这与显示链无关——同一轮的 `SURFACE`
与 `GPUFB` 亮度一致（237），说明 GPU 拷贝忠实复制了合成器的表面。

## 5. 现在能排除什么

| 假设 | 判据 | 结论 |
| --- | --- | --- |
| 显示链写错（通道序、stride、alpha、地址） | `SURFACE` 与 `GPUFB` 亮度一致、`copied 129 status 0` | 排除 |
| GPU 交接没工作 | `offered 129 done 129`、`GPUHANG h=0` | 排除 |
| 合成器不出帧 | `sw=149 own=1`，合成触摸后帧数上升 | 排除 |
| 壁纸画了但没显示 | 壁纸区域是纯 `(242,242,247)` | 排除 |
| addon 没挂上 | `[addon] 1=5/0 2=5/0 3=5/0`、`WALLPAPER stage=5 status=0` | 排除 |
| **壁纸进程没有把图放进合成结果** | 壁纸区域纯色 + CollectionsPoster 已在 `askers` | **仍成立** |

## 6. 下一步（源码优先）

1. `GpuScanoutClient.h:22` 的 `kScanoutFillProbe` 产生那帧纯白（`PANEL` 转 255 的同一行
   `imp=1/0` 是首次扫描缓冲 import）。关掉它，之后任何亮帧必然是内容。
2. 读 `[gpu objects]` 里 CollectionsPoster 所在槽的 `subs=/ibs=`：若为 0，说明它拿到设备
   却一笔都没提交，问题在它自己初始化（Turnip `VkResultCode(-3)` 在 of4 出现过）。
3. 查壁纸选择：`progress/2026-11-06` 记过「壁纸选择存在 SQLite 数据库里」。若选择指向
   本镜像不存在的 collection，即使渲染正常也不会画。
4. 判据：`SURFACEL mx` 在壁纸区域出现大面积非 `(242,242,247)`；`screen.bgrx` 的
   top-color 份额里 `(242,242,247)` 显著下降。

## 7. 持久日志的槽读数（补充）

```
[gpu clients] kernel_task 0/0/2 s- 512M r0 | mediaserverd 2/0/1 s1 4116M r0 |
  backboardd 2/0/1 s0 4543M r0 | SpringBoard 2/0/1 s2 4148M r0 |
  WidgetRenderer_Defa 2/0/1 s3 4161M r0 | CollectionsPoster 2/0/1 s4 4096M r0 |
  PhotosPosterProvide 2/0/1 s4 4096M r0 | PosterBoard 2/0/1 s4 4096M r0 | Preferences ...
[gpu objects] s0 593/602 r0 0/0 w447M subs=307 ibs=2856 | s1 10/17 subs=4 ibs=4 |
  s2 22/22 subs=5 ibs=13 | s3 56/56 subs=16 ibs=34 | s4 17/17 subs=4 ibs=4
```

`CollectionsPoster`、`PosterBoard`、`PhotosPosterProvide` 都在 **slot s4**，而 s4 读到
`subs=4 ibs=4`——它们确实提交了工作，不是"拿到设备却一笔没交"。

所以壁纸缺席既不是 GPU 没跑、也不是壁纸进程没跑，而是**它们画出来的内容没有落在屏幕的
壁纸区域**。`screen.bgrx` 的壁纸区域是均匀 `(242,242,247)`，说明那里是合成器的底色。

下一步按可能性排序：

1. 壁纸选择：`progress/2026-11-06` 记过壁纸选择存在 SQLite 数据库。若选择指向本镜像不存在
   的 collection，PosterBoard 会合成一个空/底色海报。
2. overlay 目标 bundle：官方 Flat 配方覆盖 `7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.wallpaper`。
   若 PosterBoard 在本设备 idiom 下选的是另一个变体，覆盖的文件不会被读。
3. `kScanoutFillProbe` 的纯白帧掩盖了第一帧内容，但它不影响后续帧。

判据：壁纸区域出现非 `(242,242,247)` 的像素，或 `screen.bgrx` 里 `(242,242,247)` 的
份额显著下降。

## 8. 找到壁纸读不到的源码原因

真实 bundle 的 `main.caml` 对每张图有**三种引用形式**：

```
"/assets/J120.HEIC"     assets/J120.HEIC     "J120.HEIC"
```

`NEW/UniXNU/scripts/make-wallpaper-overlay.py:53` 只替换了前两种：

```python
main_caml.replace(f'"/assets/{image}.HEIC"', f'"/{png}.png"')
         .replace(f'"assets/{image}.HEIC"', f'"{png}.png"')
```

于是 `"J120.HEIC"` 原样留下。overlay 只带 PNG，不带 HEIC，文档却仍向 ImageIO 要一个
HEIC——而这块板没有硬件 HEVC 解码器（`reference.md` 记过 `J120.HEIC` 读不出来）。
另外替换后的 `"/J120.png"` 带前导斜杠，会解析到卷根，同样找不到 `.ca` 下的文件。

实测确认：`tmp/wallpaper-official-flat/main.caml` 里 `J120.HEIC` 出现 1 次，
`"/J120.png"` 与 `"J120.png"` 并存。

修法（`c63c74d`）：三种形式全部替换成**裸文件名**，因为 overlay 的 PNG 就放在
`main.caml` 旁边（内核把 addon 的 `wallpaper/*` 挂到
`7570.iPadOS_17_Flat-1024w-1366h@2x~ipad.ca/`，见
`patches/xnu/zzy-native-gpu-addon-mount.patch:314-322`）。

重新生成后的 overlay：

```
refs: "J120-Dark.png"  "J120.png"
HEIC 引用: 0
```

这是目前唯一有源码与实测两边证据的壁纸根因。of9 用这份新 overlay 构建，用来验证。
