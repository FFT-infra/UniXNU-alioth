# GPU 交接第一次真正呈现：帧完成了，但帧数只有 CPU 拷贝的三分之一

2026-11-06 傍晚。设备仓库 `NEW/UniXNU-alioth`。真机读数，USB 串口抓取，`fastboot boot`，不写槽。

```
TAG=g1 ADDON=gpu-sy9.gpuaddon bash tmp/build-parity.sh
sh tmp/rounds-count2.sh tmp/boot-g1.img g1 1 200
```

原始抓取：`tmp/console-g11839501.txt`。这一轮 `board.toml` 打开 `sheng-fb-gpu=1`，并临时停掉
`sheng-banner=0` 腾出命令行空间；其余与 w18 相同。

## 1. 排队很久的那个实验

`board.toml` 里 `sheng-fb-gpu` 的注释写着：打开它，合成好的帧交给特权 GPU 客户端而不是 CPU 拷贝；
判据是 `PANEL`，因为内核用非缓存映射读 `0x9c000000`，GPU 写了就是变化，写了副本就恒为 911。
它还写着「这个探针轮次已排队」。

此前 mp1 轮（2026-11-04）测过 `=1`：GPU 把整个扫描输出填 0xFF，命令缓冲完成，而 `PANEL` 一个字节
都没变，于是当时的结论是 `newBufferWithBytesNoCopy` 没有别名调用方页面、GPU 写的是副本。

## 2. 读数

```
GPUFRAME ready 1 fail 0 copied 0 gpu offered 59 done 59 fail 0 abandon 0 | legacy surfaces 3 swaps 59
GPUSWAP  validate 59 ok 1 reason 0 | copied 59 status 0 bytes 10368000 | retired 59 gen 59
GPUOPEN  backboardd type 4147 macf 1 res e00002e2 attach 1 foreign 0
GPUCCLI  CollectionsPoster opens 2 refused 0 closes 1 slot 4 peak 4096 last 0
GPUCCLI  WidgetRenderer_Defa opens 2 refused 0 closes 1 slot 3 peak 4141 last 0
SUPER    t=190s sw=60/60/0 own=1 prep=60 tf=27808
```

`PANEL` 在整轮里采到五种值：`911/1000 luma 43`（桌面之前）→ `1000/1000 luma 255`（白帧）
→ `26/1000 luma 5` → `579/1000 luma 135`（桌面）。

板主确认：**桌面出来了，没有壁纸，流畅度一般。**

## 3. 结论

**GPU 交接现在是通的。** `offered 59 done 59 fail 0`，而 `sheng-fb-gpu=0` 时是 `offered 0`。
`NativeGOPresenter.cpp:122` 写着 `if(gpuMode) return kIOReturnUnsupported; // in GPU mode no CPU
copy exists`，而画面仍然出现并随界面变化，所以这一轮的画面是 GPU 写进 `0x9c000000` 的。
mp1 的"GPU 只能写副本"的结论不再适用于今天这条路径。

**壁纸渲染器第一次连上了 GPU。** `CollectionsPoster` 与 `WidgetRenderer` 各开两次、各关一次，
分别占用槽 4 和槽 3，峰值约 4 GiB 地址空间。此前 GPUCCLI 只列 `kernel_task`。

**但帧数只有 CPU 拷贝路径的三分之一。** 同为 t=190 s：

| | `sheng-fb-gpu=1`（本轮）| `sheng-fb-gpu=0`（w18）|
| --- | --- | --- |
| `sw` 接受 | 60/60/0 | 210/210/0 |
| `PANEL` | 579 luma 135 | 515 luma 125 |

板主的"流畅度一般"与这个数字一致：不是观感问题，是帧数确实低。

## 4. 边界

- 没有测出低帧数的原因。候选在 `[fence wait]` 的轮询与每次提交 50 次零超时等待（gpu 区
  `limits` 第 0 条，u260 读到的 385 us 均值、47% 的单帧预算），但那行属于持久日志，本轮没有读。
- 壁纸仍然不出现。GPU 客户端通了，所以壁纸的阻塞点不在 GPU 连接这一层。
- 本轮把 `sheng-banner=0` 停掉是临时腾空间，跑完恢复。
