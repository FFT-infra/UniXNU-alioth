# 累积过的 Data 卷上，PosterBoard 与四个 Poster 扩展都起来了（但那一轮 GPU 挂了）

2026-11-07 下午。`fastboot boot`，不写槽。镜像 `tmp/boot-acc1.img`
（sha256 `d777342bca66c58e0462f2d527bbf2376eabceb9586879c18ea040eafb87d52b`），
读数 `tmp/console-acc11527082.txt`，第二轮（`acc11527082`，跑到 t=830）。

## 1. 这一轮的配置

* 有 `sheng-data-persist=2`，**没有** `sheng-data-reset=1`——overlay 从上一轮恢复，卷上的状态累积下来。
* **撤掉了** `pipa-gpu-zap=1`（为了躲开它那条偶发挂死的 TrustZone 读）。
* `sheng-autoreset=900`，抓 840 秒。

## 2. 读数

```
GPUCLI clients=8 ... askers=12[mediaserverd,backboardd,SpringBoard,WidgetRenderer_Defa,
      CollectionsPoster,PhotosPosterProvide,UnityPosterExtensio,KaleidoscopePoster,AegirPos…]
GPUOPEN PosterBoard type 4147 macf 1 res e00002e2 attach 1 foreign 0
GPUMETAL PosterBoard reads 1
GPUHANG h=1349 r=0 f=0 fence=0 …
GPUCLIENT WidgetRenderer_Defa  no-device device: DeviceLost
GPUCLIENT CollectionsPoster    no-device device: DeviceLost
```

## 3. 两件事，一件成一件败

**成了**：`[metal askers]` 从 5–6 个涨到 12 个，`PosterBoard`、`UnityPosterExtension`、
`KaleidoscopePoster`、`AegirPoster` 都进来了。这正是 `rd9`（空白卷、跑 821 秒）没有的东西：
**PosterBoard 需要的是 Data 卷里攒出来的状态，不是某个开关。** `§58` 的结论由此确认。

**败了**：GPU 在那之前就挂了。`GPUHANG h=1349 r=0` 是一轮 840 秒里每 500 ms 记一次的同一次挂死
（`NativeAdrenoPublish.cpp:221`），因为 `sheng-gpu-norestart=1` 只记不重启；随后每个客户端都读到
`DeviceLost`。此前所有进过桌面的轮次（rd3、rd6、rd9、of18b）`GPUHANG h=0`。

差别只有一处：这一轮没有 `pipa-gpu-zap=1`。**ZAP 不是可选项**——没有它，环在某个提交上停住。

## 4. 因此

* `pipa-gpu-zap=1` 放回命令行，并写进 `board.toml` 的注释（撤它的代价是整轮 GPU 报废）。
* `sheng-data-reset=1` 保持撤掉：它每轮丢掉累积状态，而那正是 PosterBoard 唯一的启动条件。
* `acc2`（ZAP 回来、无 reset、每轮 840 秒）继续跑，看一整轮健康的累积启动里
  `CollectionsPoster` 的 `entry → queue → cmt → d…` 停在哪一级。
