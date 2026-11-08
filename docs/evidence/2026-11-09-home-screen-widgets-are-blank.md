# 锁屏第一次上滑即解锁，主屏壁纸正确、小组件是空白块

2026-11-09。设备仓库 `dfb93c0`（HID 组件 `b5b925b`）。`fastboot boot`，不写槽。
这份证据把两件事钉在一起：合成触摸第一次注入就生效，以及主屏上小组件位是纯白空块。

## 1. 轮次与参数

`hdp4` = `wp-poster=1 sheng-touch-synth=400 sheng-touch-synth-swipe=1 offline-screenshot=1 qcom-usb-ctl=1`
（1011 字节）。`tmp/round-ctl.sh` 启动，`tmp/sxnu-shot.py` 独占控制台并在看到 `TOUCHTAP`
之后 12 s 走 SXNU 控制通道取一整帧（1080x2400，RLE 3,614,168 字节，`crc32=88c66858` 校验通过）。

控制通道要 `qcom-usb-debug=1` 加 **`qcom-usb-ctl=1`**（`docs/control-and-screenshot.md`：缺少后者时
正常 worker 不开启控制接口）；只给前者时 HELLO 无应答，这正是 `hdp3` 里取图失败的原因。
传输实测约 25 KB/s，整帧约 2.5 分钟。

## 2. 控制台读数

```
SURFACEL n=3 nonblack=18/1000 luma=4 sum=b228fd55 ...      ← t=60，锁屏还没画
TOUCHSYNTH down / TOUCHTAP down n=1 x=15240 y=20277 / up   ← 本轮唯一一次触摸，t≈62
SUPER t=70s sw=26/26/0                                     ← 前一行 t=60 是 4
SURFACEL n=4 nonblack=929/1000 luma=163 sum=a66c0c90 ...   ← 主屏已画（t=70）
```

同一条交互链上，这是第一次「一轮只有一次触摸、且这次触摸就解锁」的读数。

## 3. 截图（本目录同名的 `.png`，540x1200；原帧 1080x2400）

- 壁纸是 J120 的彩色波形，整屏无黑边；状态栏在顶部；Dock 五个图标（信息、Safari、照片、
  Settings、一个文件夹）都在。
- **小组件位是三块纯白圆角矩形**（顶部一块 4x2、中部一块 2x2），没有任何内容；
  旁边只有 Settings 和一个被白块压住的蓝色图标。
- 这与 grid1 的 unified log 一致：`ATXWidgetDescriptorCache init - descriptor count: 0`、
  `Error unarchiving data and no migration is necessary: (null)`，以及十六行
  `Missing intent and no descriptor intent found for widget: … descriptor: (null)`。
  白块就是 SpringBoard 给「有位置、没有描述符」的小组件画的占位图。

## 4. 结论与边界

- 触摸链路：**完成**。第一次合成上滑 → 锁屏离开 → 主屏渲染，端到端可复现（hdp2、hdp3、hdp4 三轮）。
- 壁纸：**完成**。`display-scale=2` 之后主屏/锁屏都是完整壁纸。
- 小组件：**未完成，且根因在描述符缓存**。下一步是查 `ATXWidgetDescriptorCache` 为什么是空的
  （ExtensionKit/pkd 的发现、或 Data 卷里那份缓存文件），而不是抓帧或触摸。
