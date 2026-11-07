# 去掉 ZAP PAS 序列后：Metal 发布、客户端接入、第一笔客户端 IB 挂住

2026-11-07。设备仓库 `NEW/UniXNU-alioth`。真机读数，USB 串口抓取，`fastboot boot`，不写槽。

## 1. 本轮改动

| 提交 | 内容 |
| --- | --- |
| `89d956e` | `board.toml` 去掉 `pipa-gpu-zap=1` |
| `231e1e9` | `metal()` 不再由自检门禁，与官方 `GpuPowerProbe.cpp:1776` 一致 |
| `b374857` | 自检判据只数三个 CP 标记，不再数 CPU sentinel |
| `ae4cc73` | 采用官方最新 GPU 诊断脚本 |
| `ed1d2ba` | `devices.toml` 恢复官方 265 条并清掉 diff3 基线行 |
| `95e7ac2` | 采用官方最新 GPU 诊断 / 驱动 / 启动参数文档 |

镜像 `tmp/boot-of4.img`，sha256
`0f1763bebad7519cd0b240f6e2479ec3fcc3fc593445f7d141fd71a599085c32`，`LINK_OK`，
addon `gpu-official-flat.gpuaddon` `d484c4fa74c2cf2e`，命令行 983 字节。

## 2. 为什么去掉 ZAP

of3 的串口只有一次 `ZAPSCM enter copy`，没有 `done read`，整轮 0 条 `GPUROW`：

```
ZAPSCM enter init_image
ZAPSCM done init_image 0/0 7 ms
ZAPSCM enter mem_setup
ZAPSCM done mem_setup 0/0 0 ms
ZAPSCM enter copy
```

`GpuProbe.cpp:1223-1229` 的注释已写明：`pil_gpu_region` 在认证后归 TrustZone，
AP 侧的读会挂住，并把整条 GPU 探针一起带走。bootloader 已经装过 zap，内核这条 PAS
序列是冗余的（`progress/2026-11-03-gpu-chain.md` 第 11 节：nozap 进桌面、`METAL ok`）。
`col2`（同树、无 zap）也是 `METAL ok` 加 5 个 GPU 客户端。

## 3. of4 读数

```
GPUROW ZAP nozap step 0 init 0/0 mem 0/0 auth 0/0 resumes 0 ms 0/0/0 phdrs 0 reloc 0 span 0 secure 0 meta 0000
GPUROW IRQ attach 0 readback a002 router 0 count 1>2 nop fence 1 seen 00100000
GPUROW23 PUBLISH ok ... uc 36/36 first 0 at 0 cpumap 0 | rounds 3 3 | tgt badc0de feed0001 600d0003 deadbeef | tgt1 badc0de feed0001 600d0003 deadbeef | rp 0
GPUROW24 METAL ok: accelerator registered (IOGPUFamily loaded, class size 136), Turnip options nocb
```

- 自检两轮各 3 个 CP 标记全到，`rounds 3 3` 是新判据的通过值。
- `GPUCLI` 到 `askers=9`：`backboardd, mediaserverd, PhotosPosterProvide, SpringBoard,
  WidgetRenderer_Defa, CollectionsPoster, PosterBoard, KaleidoscopePoster, AegirPoster`。
- 桌面交换 `own=1`，`SUPER t=290s sw=3/3/0`。

**但零拷贝没有完成**：

```
GPUFRAME ready 1 fail 0 copied 0 gpu offered 1 done 0 fail 0 abandon 0 | legacy surfaces 1 swaps 1
GPUSWAP validate 3 ok 1 reason 0 | copied 0 status -1 bytes 0 | retired 1 gen 1
GPUROW26 HANG st00800005 0/0/0 r1e7/221 ib1 100027000+3f4 ib2 10002c000+0 hw0 cpi0 prot0
  | sqe pc2cd $01 70380007 $02 00000504 $03 00000020 $04 00000000 $05 00000001 $06 00000000
  | roq2 2000140 st1 8000471f scr0 138 | fsr400 far00005e09655794f5
GPUROW25 LIVE ... sub 13/8 t3053 p0 | uc 64/6 | bo 140/140 r0 s0 | busy 1000 | cp 1e7/27000/3f4
  | rbbm 00800005 | scr b | rw 1e7/29f | roqavail a9002c/e20039
```

`gpu offered 1 done 0`：第一帧交出去，从没完成。`sw=3` 是 CPU 拷贝路径的交换，
`copied 0` 说明 GPU 交接把 CPU 拷贝摘掉了，于是面板停在开机图 `911`。

客户端侧同时报设备丢失：

```
WidgetRenderer_Defa:no-device device: DeviceLost | PosterBoard:no-device device: DeviceLost |
```

所以壁纸仍然没有出现，原因不是 addon 挂载（`WALLPAPER stage=5 status=0`、
`A files=17 wr=0 same=17`），而是客户端的第一笔 IB 挂住、设备被判丢失。
`CollectionsPoster` 拿到过设备但同样落在这条路径上。

## 4. 与历史读数的关系

同一形态在 `tmp/console-rs22211.txt` 出现过（`ib1 100027000+0`，`st00800005`，
`busy 999`，`fsr400`）。`progress/2026-11-01-gpu-client-ib.md` 记录了这条：
客户端 IB 的 `ib2` 落在 16 MiB 未映射区，sysmem 模式的 GMEM 地址没有映射，
`fsr 400` 只有 FORMAT 位、没有 TF/PF。该文件同时记录了修好后的形态
（`sub 25/25`、`roqavail 0/0/0`、`GPUROW26 HANG` 消失），差别是
`native_adreno_wc_enabled=0`（每笔提交前清 cache）与 `RingNoUpdate`/`fence_[0]` 影子读。

本轮 `board.toml` 已经是 `sheng-gpu-wc=0`，所以 wc 不是这里的差别；剩下的候选是
Turnip 的 sysmem/GMEM 选择（`pipa-tu=` 组）与 IB2 的地址来源。

## 5. 下一步（源码优先）

1. 读 `GPUROW26` 的 `ib2 10002c000` 与 `NativeAdrenoSubmit` 的 IB2 组装，确认它来自
   Turnip 的哪个 pass、为什么落在 16 MiB 未映射窗口。
2. 对照 `reference/repo/linux-mainline` 的 `a6xx_gpu.c` 与厂商树，确认 IB2 的
   IOVA 必须落在哪个范围。
3. 只在源码走通后再上板；一次取齐 `pipa-tu=` 的 sysmem/GMEM 对照与 `GPUFRAME`/`GPUSWAP`
   /`GPUROW26` 三组读数。
