# 零拷贝 GPU：安全态退出是必要条件（of4 / of5 两轮反向证据）

2026-11-07。设备仓库 `NEW/UniXNU-alioth`。真机读数，`fastboot boot`，不写槽。

## 1. 结论

`pipa-gpu-zap=1` 不能去掉，但也不能只发 `CP_SET_SECURE_MODE(0)` 而不装载镜像。
两轮实测各自证伪了一条替代方案：

| 轮次 | GPU 安全态 | Metal | 客户端 | 零拷贝 |
| --- | --- | --- | --- | --- |
| of4（完全不跑 zap） | CP 留在安全态 | `METAL ok` | 9 个 asker，5 个拿到设备 | 第一笔 IB 挂住，`offered 1 done 0` |
| of5（跳过拷贝、仍发 secure 退出） | 安全世界拒绝，`secure 1` | 未注册 | 0 | 无 |
| g1（`zap=1`，拷贝完成） | 退出成功 | `METAL ok` | CollectionsPoster、WidgetRenderer 等 | `offered 59 done 59` |

## 2. of4：Metal 能发布，客户端 IB 挂住

```
GPUROW ZAP nozap step 0 init 0/0 mem 0/0 auth 0/0 resumes 0 ms 0/0/0 phdrs 0 reloc 0 span 0 secure 0 meta 0000
GPUROW23 PUBLISH ok ... rounds 3 3 | tgt badc0de feed0001 600d0003 deadbeef
GPUROW24 METAL ok: accelerator registered (IOGPUFamily loaded, class size 136), Turnip options nocb
GPUCLI clients=5 ... askers=9[backboardd,mediaserverd,PhotosPosterProvide,SpringBoard,
  WidgetRenderer_Defa,CollectionsPoster,PosterBoard,KaleidoscopePoster,AegirPoster]
GPUFRAME ready 1 fail 0 copied 0 gpu offered 1 done 0 fail 0 abandon 0 | legacy surfaces 1 swaps 1
GPUSWAP validate 3 ok 1 reason 0 | copied 0 status -1 bytes 0 | retired 1 gen 1
GPUROW26 HANG st00800005 0/0/0 r1e7/29f ib1 100027000+3f4 ib2 10002c000+0 hw0 cpi0 prot0
GPUROW25 LIVE ... sub 13/8 t3053 p0 | uc 64/6 | busy 1000 | cp 1e7/27000/3f4 | rbbm 00800005
[fence wait] calls=6864 timeouts=6786 last timed-out fence=12 gpu wrote=11 submitted=16 completed=11
[scanout wait] calls=30 timeouts=29 asked=500ms
WidgetRenderer_Defa:no-device device: DeviceLost | PosterBoard:no-device device: DeviceLost
```

客户端确实进了 GPU（9 个 asker，5 个拿到设备），第一笔客户端 IB（`ib1 100027000`，736 字节）
被 CP 取走后没有退役，`busy 1000`，`fsr 400`，随后所有客户端读到 `DeviceLost`。
面板停在开机图 `911`，壁纸因此不可能出现。

持久日志与诊断导出齐全：`build/sheng-logs/of4-00000009d5fd198f`，
`diag 557 files`，含 `prime.log`（157/157 库预热完成）与 `screen.bgrx`。
预热完成说明 shader 侧不是这里的瓶颈。

## 3. of5：安全世界拒绝空手退出

```
GPUROW ZAP secure exit timeout step 0 init 0/0 mem 0/0 auth 0/0 resumes 0 ms 0/0/0
  phdrs 0 reloc 0 span 0 secure 1 meta 0000
```

`secure 1` 表示 `CP_SET_SECURE_MODE(0)` 之后仍未退出安全态，`leaveSecure()` 超时。
整轮 `METAL ok` 出现 0 次，`askers=0`。所以「不装载镜像、只发退出」这条在本轮不成立。

## 4. 正确的下一步

`zapLoad` 的拷贝在 g1、of1、of1b、w18 四轮都完成，只有 of3 卡在第一次读
（`ZAPSCM enter copy`，无 `done read`）。所以问题是**偶发的区域读挂住**，不是序列本身。
修法应落在 `package/GpuProbe.cpp:1232-1252`：

- 读之前先把 `pil_gpu_region` 的可访问性判定做成有界操作，读失败或超时后走主线分支
  `RBBM_SECVID_TRUST_CNTL = 0`（`reference/repo/linux-mainline/drivers/gpu/drm/msm/adreno/a6xx_gpu.c:1516`），
  而不是把整条 GPU 探针带走。
- 失败分支必须写进 `ZAP` 行（例如 `copy unreadable -> trcntl0`），让下一轮从一行读出走了哪条。

判据（一轮取齐）：

- `GPUROW ZAP` 行不以 `in copy` 停住；
- `GPUROW24 METAL ok`；
- `GPUCLI` 里 `CollectionsPoster` 与 `WidgetRenderer_Defa` 不是 `no-device`；
- `GPUFRAME gpu offered/done` 同步增长；
- `PANEL` 离开 911；
- `diag` 里的 `screen.bgrx` 出现非中性色（壁纸像素）。

## 5. 本轮其它已修项

- `231e1e9`：Metal 注册不再由自检门禁，与官方 `GpuPowerProbe.cpp:1776` 一致。
- `b374857`：自检只数三个 CP 标记（of4 读到 `rounds 3 3`）。
- `ed1d2ba` / `95e7ac2`：`devices.toml` 恢复官方 265 条并清掉 diff3 行；五份官方文档同步到 `upstream/main`。
- `ae4cc73`：采用官方最新 GPU 诊断脚本（`check-gpu-trace.py` 等）。
- `50b6121`：`board.toml` 保留 `pipa-gpu-zap=1`，注释写入上述三轮的读数理由。
