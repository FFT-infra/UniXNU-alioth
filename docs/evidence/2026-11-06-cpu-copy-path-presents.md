# CPU 拷贝路径能呈现：`sheng-dpu-scanout=0` 的第一轮真机读数

2026-11-06。设备仓库 `NEW/UniXNU-alioth`，提交 `80cf590`（`board.toml` 改回官方默认）。
真机读数，USB 串口抓取，`fastboot boot`，不写槽。命令：

```
TAG=w18 ADDON=gpu-sy9.gpuaddon bash tmp/build-parity.sh
sh tmp/rounds-count2.sh tmp/boot-w18.img w18 1 200
```

原始抓取：`tmp/console-w18*.txt`。

## 1. 为什么这一轮之前 `scanout=0` 被判"放不出东西"

2026-10-29 的三次 `scanout=0` 启动都读到 `ds=1, own=0, sw=0`、没有 `SURFACEL` 行。当时读成
"CPU 呈现路径在这块板上不可用"。

那三次启动每次都在七秒左右丢一颗核（原因见
`2026-11-06-the-deaf-core-is-the-gpu-freq-poll.md`），渲染服务根本没起来，所以 `sw=0`
不是呈现路径的结论，是启动失败的副产品。

## 2. 读数

```
ds=1  sw=84/84/0  prep=168  own=1  tf=13377  tlow=63  TOUCHTAP=14
PANEL nonblack=515/1000 luma=125 rgb=124/125/125
deaf=00
```

到 `t=190s`：`sw=210/210/0`、`prep=420`、`tf=27596`。

- `sw` 的 84 与 `prep` 的 168 是精确 2:1，与直通扫描失控时的 `prep=121717 / sw=4` 形成对照。
- `PANEL=515/1000 luma=125` 是探针第一次读到真正的桌面画面；直通扫描下这个值恒为 911，
  因为面板扫出的是探针看不见的 surface。
- `own=1`：`boot_progress_display_owned`，显示所有权确实交到了用户态。
- `deaf=00`：整轮没有丢核。

板主在本轮确认：桌面可见，触控可用。

## 3. 对照

| | `ds=4`（`scanout=2`，w16）| `ds=1`（`scanout=0`，w18）|
| --- | --- | --- |
| 触摸后屏幕 | 停在最后一帧 | 正常跟随 |
| `prep` / `sw` | 121717 / 4 | 168 / 84 |
| `PANEL` | 911（读不到画面）| 515（真画面）|

两轮的 `TOUCHTAP`（46/48 与 14）和 `TOUCHSTAT`（`contacts=0 irq=1 hstage=8`）一致，所以差别
不在输入侧。

## 4. 结论与边界

- 官方默认值 `sheng-dpu-scanout=0` 在这块板上能呈现，`porting/display` 的显示里程碑
  （发布显示服务、呈现画面、完成交换）由这一轮满足。
- 该里程碑的第二半（背光控制）不满足：亮度驱动受 DSI0 中断与 secure_mode 限制，见 display 区。
- 直通扫描的触摸后失控只被绕开，没有解释；它仍是 display 区的 `next_action`。
