# 零拷贝第一次真正完成，壁纸仍未出现（of6）

2026-11-07。设备仓库 `NEW/UniXNU-alioth`。真机读数，`fastboot boot`，不写槽。

## 1. 本轮配置

镜像 `tmp/boot-of6.img`，sha256
`3d552d16defa9dd76b40736c5b05bc3f97a854a017c787ca7f63f9f2923fe557`，`LINK_OK`。
命令行 983 字节，含 `pipa-gpu=12 sheng-gpu-wc=0 pipa-gpu-zap=1 sheng-gpu-norestart=1
sheng-gpu-addon=1 sheng-fb-bgra=1 sheng-fb-gpu=1 sheng-gpu-trace=1 sheng-gpu-perf=1
sheng-diag-export=240`。addon `gpu-official-flat.gpuaddon` `d484c4fa74c2cf2e`。

设备仓库本地提交链：`231e1e9`（Metal 不再由自检门禁）、`b374857`（自检只数三个 CP 标记）、
`89d956e` → `5094ba3` → `50b6121`（ZAP 开关的三轮实测取舍）。

## 2. 零拷贝完成

```
GPUROW ZAP ok step 7 init 0/0 mem 0/0 auth 0/0 resumes 0 ms 7/0/0 phdrs 3 reloc 1 span 1000 secure 0 meta d75bc000
GPUROW24 METAL ok: accelerator registered (IOGPUFamily loaded, class size 136), Turnip options nocb
GPUFRAME ready 1 fail 0 copied 0 gpu offered 12 done 12 fail 0 abandon 0 | legacy surfaces 3 swaps 12
GPUSWAP validate 12 ok 1 reason 0 | copied 12 status 0 bytes 10368000 | retired 12 gen 12
GPUROW25 LIVE irq63 ... sub 61/61 t134 p0 | uc 20/13 | bo 366/366 r0 s0 | busy 0
  | cp a20/27000/0 | rbbm 00000001 | scr 41 | rw a20/a20 | roqavail 0/0/0
GPUHANG h=0 r=0 f=0 fence=0 st=0 rp=0 g=0 kv=0 vm=7/7/18 tst=0/0 flt=0/0 rb=a20/a20 sc=61/60
```

- `copied 0` 且 `gpu offered 12 done 12`：帧由 GPU 写进显示内存，CPU 拷贝已移除。
- `sub 61/61`、`rw a20/a20`、`roqavail 0/0/0`、`GPUHANG h=0`：环被读空，没有挂起。
  这与 of4 的 `sub 13/8`、`busy 1000`、`fsr 400` 是相反的形态。
- 自检第一轮返回 0（`rounds 0 3`），第二轮 3 个 CP 标记全到；按新判据 `0 3` 仍是失败，
  但已不阻止 Metal 注册。第一轮为 0 的原因尚未定位，单独记录。

## 3. 客户端

```
GPUCLI clients=6 ... askers=10[mediaserverd,backboardd,SpringBoard,WidgetRenderer_Defa,
  CollectionsPoster,PhotosPosterProvide,PosterBoard,UnityPosterExtensio,GradientPosterExten,
  EmojiPosterExtensio]
txt=... SpringBoard:S c2 d2 D0 ... | WidgetRenderer_Defa:S c6 d6 D12 ... |
     UnityPosterExtensio:no-device device: Vulkan(VkResultCode(-3)) |
```

比 of4 多：四个 Poster 扩展都来要设备，CollectionsPoster 与 PosterBoard 不再报
`DeviceLost`。只有 `UnityPosterExtension` 仍拿不到设备（`-3`，Turnip 初始化失败）。

## 4. 画面：先白后黑

```
PANEL 911/1000 luma 43   (开机图)
PANEL 1000/1000 luma 255 (纯白)
PANEL 26/1000 luma 5     (近黑，之后一直停在这里)
GPUFB  checksum 4dccbc6e changes 6 (+3) nonblack 28/1000 luma 6
```

面板先出现一帧纯白，再落到近黑。`GPUSWAP copied 12` 说明 12 帧确实进了扫描缓冲，
但最终停在暗屏。所以零拷贝的传输链通了，**画面内容不对**：这不是「没画」，而是「画成了黑的」。

`PANEL` 行里 `gpu off=9 comp=9`：GPU 交接生效，CPU 拷贝按设计摘除。

## 5. 壁纸仍未出现

- addon 与 overlay 都成功：`[addon] 1=5/0 2=5/0 3=5/0`、`WALLPAPER stage=5 status=0`。
- CollectionsPoster 已连上 GPU 且不再报 `DeviceLost`，但面板是近黑，没有任何壁纸像素。
- of4 的诊断导出（本轮 `sheng-diag-export=240` 未生效，`diag_bytes 0`）里
  `prime.log` 读到 157/157 库预热完成，`screen.bgrx` 与 of6 的 `GPUFB` 一致为近黑。
  所以 shader 预热不是瓶颈。

结论：**GPU 链已通，壁纸层仍没有被合成到显示帧**。下一步要区分「合成器画了黑背景」
和「壁纸层根本没画」，用 `GPUFB` 的 `sides`、PosterBoard 的层记录和
`sheng-wallpaper-posters` 的 job 状态三者一起看，而不是再猜 GPU。

## 6. 本轮暴露的两个新问题

1. `sheng-diag-export=240` 在 of6 没有产出（`diag_bytes 0`）。of4 同样参数却有 557 个文件。
   差别是 of6 的运行更短或导出线程被抢占；需要把导出时间提前并读 `BOOTLOG` 的 phase 行确认。
2. 自检第一轮 `rounds 0`：第二轮正常。需要单独查第一轮的返回路径（`matched` 为 0 说明
   fence 或读回没完成），这与 of1/of1b 的 `rounds 1` 是同一族现象，但更严重。

## 7. 判据（下一轮）

- `GPUFRAME gpu offered/done` 同步增长（已有）；
- `GPUFB nonblack` 离开 911 后**不落到 <100**，`sides > 0`（壁纸层出现）；
- `PANEL luma` 稳定在壁纸应有的亮度，而不是 5；
- `diag` 里的 `screen.bgrx` 出现壁纸颜色，而不是近黑；
- `GPUROW23` 两轮都 `3 3`（自检第一轮的 0 需要修掉）。
