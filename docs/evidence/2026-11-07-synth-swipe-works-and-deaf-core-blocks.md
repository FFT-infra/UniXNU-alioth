# 合成上滑让无人值守达到 231 帧；失聪核挡住后续三轮

2026-11-07。设备仓库 `NEW/UniXNU-alioth`。`fastboot boot`，不写槽。

## 1. 修好两处源码缺陷后，合成手势第一次真的生效

`kits/alioth-touch/FocalTouchProbe.cpp` 的两处改动（提交见仓库）：

1. 合成触点移到模块作用域 `static struct Fts5652Contact synthContact`。
   原来是块内局部变量，`c = &tap` 后块结束，`c[i]` 读的是失效栈槽——of8 因此
   报出面板幻影值 `x=0 y=20319` 而不是设定的中心。
2. 计时从**第一次 `publish()`** 起算（新增 `synthFirstMs`）。原来与
   `mach_absolute_time` 直接比较，而该计数开机时已是 550 s，触点在**第一帧**就触发。
3. 新增 `sheng-touch-synth-swipe=1`：从底边上滑回主屏，而不是中心长按
   （中心长按会打开应用，of8 就是这样打开了 Settings）。

## 2. of11：无人值守第一次达到 231 帧

```
TOUCHSYNTH cfg hold=4000 at=60000 swipe=1
TOUCHSYNTH down
TOUCHTAP down n=1 x=15240 y=20277 t_us=1845693880     ← 底边，x 居中
TOUCHSYNTH up
SUPER t=60s  sw=0/0/0    own=0        ← 触点之前，合成器没动
SUPER t=70s  sw=44/44/0  own=0
SUPER t=80s  sw=145/145/0 own=0
SUPER t=90s  sw=225/225/0 own=0
SUPER t=390s sw=231/231/0 own=0
RESULT t=390 own=1 sw=231 data=2 lit=976 ds=1 panel=981 frame=224 askers=8 -> desktop
SURFACEL n=1 nonblack=575/1000 luma=135 mx=26/197/251 sp=225
```

`sp=225` 是**第一次出现饱和彩色**（of8 的 `sp=5` 是纯灰）。帧数由合成手势驱动，
不是真手指：整轮 `TOUCHTAP` 只有合成那一次。

## 3. of12 / of12b / of13：失聪核挡住桌面

三轮都停在 `no-desktop`。共同的读数：

```
CORES t=10s  c4=15143/2295   deaf=00
CORES t=30s  c4=15143/2295   deaf=1e      ← c4 冻结
CORES t=160s c4=15143/2295   deaf=1e
TOUCHSTAT t=3s frames=99 ... hid=0        ← 触摸轮询线程死了，之后再无 TOUCHSTAT
SUPER ... calls=6 sw=0/0/0 own=0
GPUCLI clients=0 ... askers=0[]
```

对照 of11：

```
CORES t=10s  c4=428660/262522  deaf=00
CORES t=400s c4=3125522/...    deaf=00
TOUCHSTAT 81 行，frames=57851
askers=8
```

c4 是触摸轮询线程所在的核。它一冻，`publish()` 不再被调用，`synthTap()` 永不推进，
合成手势也永远不会触发——这是连锁的。

## 4. of12 第一轮另有一层：ZAP 拷贝停滞

```
ZAPSCM enter copy          ← 没有 done read / done copy
GPUROW 0 行
```

这是已知偶发（`reference.md` §36.3：`zapLoad` 不是稳定卡点，同一镜像三轮三种结果）。
of12b 重试同一镜像即通过（`done read`、`done copy` 都出现）。**不是新问题，不改代码。**

## 5. 变量对照（只到相关性，不是因果）

| 轮次 | `boot-ui-trace=1` | `alioth-bt/bl=1` | `deaf` | 桌面 |
| --- | --- | --- | --- | --- |
| of11 | 有 | 有 | `00` | 是，sw=231 |
| of12 / of12b | 无 | 无 | `1e` | 否 |
| of13 | 无 | 有 | `1e` | 否 |

of13 恢复了 `alioth-bt/bl` 但**没有**恢复 `boot-ui-trace`，仍然 `deaf=1e`。
所以差异落在 `boot-ui-trace` 上，但这是一组变量、三轮，**只能说相关**。

已把两个参数都放回 board.toml 默认值。of14 用来验证。

## 6. 命令行预算

`unixnu` 的上限是 1023 字节（`payload.py:35`，`DarwinBoot.c:519` 的 `CHAR8[1024]`）。

`--kernel-arg NAME=VALUE` 会**按名替换**而非追加（`payload.py:114-117`），所以
`sheng-disable-jobs=x` 把 83 字节换成 21 字节，腾出 61 字节——这是之前算错的地方，
当时以为两个探针参数会超限。

当前 board 基线 1006 字节；of14 加 `sheng-disable-jobs=x` + `sheng-touch-synth-swipe=1`
+ `sheng-diag-export=150` = 1013 字节，余量 10。
`sheng-wallpaper-db=1`（21 字节）需要单独一轮，或再腾 21 字节。

## 7. 下一步

1. of14 验证 `boot-ui-trace` 是否真的让 c4 活下来（`deaf=00` + `sw>200`）。
2. 若 of14 通过，用同一镜像带 `sheng-diag-export` 读截图，确认画面是**主屏**而不是 Settings。
3. 壁纸数据库探针单独一轮，命令行需要再腾 21 字节。
