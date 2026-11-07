# of8 的画面是 Settings，不是主屏：壁纸判据一直测错了对象

2026-11-07。设备仓库 `NEW/UniXNU-alioth`。读数来自本地 `tmp/console-of80314351.txt`
与 `build/sheng-logs/of8-0000000276faef5f/`，不是新的一轮板子。

## 1. 结论

`reference.md` §55 与 `docs/evidence/2026-11-07-synthetic-touch-desktop-no-wallpaper.md`
把 of8 的 `SURFACEL` 壁纸区域当成"合成器底色的纯 `(242,242,247)`"，据此判定
"壁纸层没有画进合成结果"。**这条判据无效**：of8 导出的 `screen.bgrx` 是
**Settings 全屏**，不是主屏。`(242,242,247)` 是 iOS 的 `systemGroupedBackground`，
即设置页的分组表格底色。

同一轮的三条读数一起把这件事定死：

| 读数 | 值 | 含义 |
| --- | --- | --- |
| `screen.bmp` | Settings 页（标题 "Settings"、Sign in to your iPad、Airplane Mode…） | 画面是应用，不是桌面 |
| `SURFACEL n=1` | `mx=242/242/247 sp=5` | 饱和度 5/255，近乎纯灰，不是图像 |
| `TOUCHTAP` ×4（t≈61s） | `x=15536..26243 y=18558..19421` | 落点 y≈0.93，底部 dock 区 |

## 2. 时间线（控制台）

```
t=10..60s  SUPER sw=0/0/0 own=0         屏幕暗（PANEL luma=43）
t=50s      PANEL luma=255 imp=1/0       kScanoutFillProbe 的首次扫描缓冲 import
t=61s      TOUCHTAP down ×4            真实手指，落点 y≈0.93
t=70s      SUPER sw=78 own=1            开始出帧
t=70s+     PANEL luma=236..238          Settings 的分组背景
```

前 60 秒 `own=0 sw=0`：合成器没有 owner。出帧是在 t=61s 的手指之后开始的，
不是合成触摸带来的。

## 3. 合成触摸从头到尾没有生效

`sheng-touch-synth=4000` 在 of8 的命令行里（`tmp/build-of8.log` 可见），
`TOUCHSYNTH cfg hold=4000 at=60000` 也打了。但：

```
TOUCHSYNTH down
TOUCHTAP down n=1 x=0 y=20319 t_us=550420918     ← 落点是面板幻影，不是中心
TOUCHSYNTH up
TOUCHTAP up held_us=4023616
```

- 代码设的是 `tap.x = FTS5652_AXIS_X/2`（4320）、`tap.y = FTS5652_AXIS_Y/2`（9600）。
- 报出来的是 `x=0 y=20319`，正是 `TOUCHSTAT ... xy=0,20319` 的**面板幻影接触**值。
- `TOUCHSTAT t=60s contacts=0`：HID 侧从未看到这个合成触点。

两个独立的源码缺陷，都能从读数直接读出：

**缺陷 1：`tap` 是块内局部变量，地址逃逸后失效。**
`struct Fts5652Contact tap` 声明在 `{ ... }` 里，`c = &tap` 后块结束，
后面 `c[i].x` 读的是已失效的栈槽。这正是 `TOUCHTAP` 报出幻影值的形态。

**缺陷 2：时间基准错了。**
`synthAtMs=60000` 与 `absolutetime_to_nanoseconds(mach_absolute_time())` 比较，
而该计数在开机时已是 `550420918` us（≈550 s）。所以 `nowMs < synthAtMs` 在
**第一帧**就不成立，合成触点在开机瞬间触发，那时锁屏还占着显示。

## 4. 已改（本地，未上板）

`kits/alioth-touch/FocalTouchProbe.cpp`：

1. 合成触点移到模块作用域 `static struct Fts5652Contact synthContact`，`memset` 清零。
2. 计时改为从**第一次 `publish()`** 起算：新增 `synthFirstMs`，
   `nowMs - synthFirstMs < synthAtMs` 才等待。
3. 新增 `sheng-touch-synth-swipe=1`：触点改为**从底边上滑**（y 从
   `AXIS_Y-40` 走到一半，`synthSwipeMs=300`），而不是中心长按。中心长按会
   打开应用（of8 就是打开了 Settings）；底边上滑请求回主屏，不改变别的状态。

## 5. 下一轮的判据（必须是主屏）

一条读数就能分开「壁纸没画」与「测错了对象」：

1. `screen.bgrx` 的顶部不能出现 `Settings` 字样或 `Sign in to your iPad`。
2. 主屏的 dock 区应出现多个应用图标，而不是表格行。
3. 只有在这个前提下，`SURFACEL mx` 与 `(242,242,247)` 份额才有意义。

在此之前，任何关于壁纸的结论都不成立。
