# 第一次合成触摸被丢：十根手指共用一个 Contact Identifier

2026-11-09。设备仓库 `278e9fb`，HID 组件 `ShengHidInput` `b5b925b`。`fastboot boot`，不写槽。

## 1. 现象：每轮只有一次触摸，而每一次都被吃掉

`hdp1` = `scl2` + `sheng-touch-synth=400 sheng-touch-synth-swipe=1 sheng-synth-repeat=30 hidump=1`。
同一次上板每 30 s 一次底边上滑：

```
TOUCHSYNTH down                    ← 第一次，t≈61s
TOUCHTAP down n=1 x=15240 y=20277
SUPER t=60..90  sw=18/18/0         ← 30 秒内合成器一帧没多
TOUCHSYNTH repeat after 30s        ← 第二次，t≈92s
SUPER t=100 sw=59/59/0             ← +41 帧
SURFACEL n=9 nonblack=1000/1000 luma=125 mx=243/81/2 sum=d813cbb2
TOUCHSYNTH repeat after 30s
SUPER t=130 sw=86/86/0             ← 第三次再 +27 帧
SURFACEL n=12 nonblack=985/1000 luma=160 mx=253/3/117 sum=228a4723
```

`hidump=1` 打印事件驱动真正派发的对象。第一次触摸的第一份报告派发了三个子事件：

```
TOUCHDUMP collection mask=00000007 touch=1 index=0 id=0 x=32769 y=65399
TOUCHDUMP finger     mask=00000007 touch=1 index=0 id=0 x=32769 y=65399
TOUCHDUMP finger     mask=00000003 touch=0 index=0 id=0 x=0     y=0      ← 幻影抬指
TOUCHDUMP collection mask=00000007 touch=1 index=0 id=0 x=32769 y=64635
```

第二次触摸没有那条 `touch=0 x=0 y=0`，直接 `mask=00000007` 的 begin 之后就是 `mask=00000004` 的 move。

## 2. 根因

`NativeHidInput::touchSend` 只给按下的手指写 Contact Identifier，其余九个槽位保持 0。
事件驱动按标识符维护接触点（dump 里所有事件的 `index` 都是 0，因为标识符都是 0），
于是在第一份报告里初始化全部十个转导器时：

1. 转导器 0（tip=1、标识符 0）→ begin，位置 (0.5, 0.998)；
2. 转导器 1（tip=0、标识符 0）→ 同一个接触点被抬到 (0,0)；
3. 转导器 2..9 → 该接触点已抬起，不再产生事件；
4. 下一份报告转导器 0 再次 tip=1 → 重新 begin。

之后的报告只处理状态变化的转导器，幻影不再出现。所以**只有一轮的第一次触摸**是坏的，
而此前每一轮都只注入一次触摸。

## 3. 修法

参考实现（`tmp/refs/UniXNU-umi/package/touch/UmiFtsCore.h:35`）给**每个槽位**写自己的编号。
`touchSend` 改成同一规则：标识符 = 槽位号 0..9；按下的手指固定占自己的槽位（生产者给出的
接触 id < 10，直接用 id 当槽位）；抬指在同一槽位、最后位置发 tip-up。十个转导器因此永远
不会共用一个标识符。

## 4. `hdp2` 验证（同一组参数，只换源码）

```
TOUCHTAP down n=1 x=15240 y=20277
TOUCHDUMP collection mask=00000007 touch=1 index=0 id=0 x=32769 y=65399   ← 干净 begin
TOUCHDUMP finger     mask=00000007 touch=1 index=0 id=0 x=32769 y=65399   ← 无幻影
TOUCHDUMP collection mask=00000004 touch=1 ...                            ← 直接 move
SUPER t=60s sw=13/13/0
SURFACEL n=3 nonblack=916/1000 luma=111 ...                               ← 锁屏
TOUCHSYNTH down / up                                                      ← 第一次上滑
SUPER t=70s sw=46/46/0                                                    ← +33 帧
SURFACEL n=4 nonblack=985/1000 luma=160 sum=228a4723 ...                  ← 锁屏离开
```

同一次上板后续每 30 s 的上滑继续出帧（t=100 sw=74、t=130 sw=111），两屏交替
（主屏 `sum=228a4723`、另一屏 `sum=226b72eb/8970941a`），`TOUCHSTAT` 的
`edge=1,5,5` 说明每次 begin 都带底边标记。整轮判定 `RESULT t=290 own=1 sw=279 -> desktop`。

## 5. 边界

- 只验证了单指。多指（`downCount>=2` 的分支）在新编码下尚未上板。
- `hdp1` 之前所有「合成触摸无效」的轮次都作废：它们只注入一次触摸，而那一次必然被吃掉。
  `of11` 的「合成手势把 sw 推到 231」是同轮 t=60 之后成串真手指触摸的结果，不是合成手势。
