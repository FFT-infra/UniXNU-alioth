# 官方 Flat 壁纸 addon 与零拷贝默认：两轮真机读数

2026-11-06。设备仓库 `NEW/UniXNU-alioth`。真机读数，USB 串口抓取，`fastboot boot`，
不写槽。所有改动只在本地 `test` 分支。

## 1. 构建与镜像

```
TAG=of1 ADDON=gpu-official-flat.gpuaddon bash tmp/build-parity.sh
sh tmp/rounds-count2.sh tmp/boot-of1.img of1  1 240
sh tmp/rounds-count2.sh tmp/boot-of1.img of1b 1 240
```

- 镜像 `tmp/boot-of1.img`，sha256
  `48eba2794f15070d553232f42c0de43ac0f987c7e7232241659d1099ac4d3828`。
- addon 为 `tmp/gpu-official-flat.gpuaddon`，sha256
  `d484c4fa74c2cf2e0c700e7b1e4987e49b93bf47a2b354b518823d955ab382a0`。
- 构建日志 `tmp/build-of1.log`，结束标志 `LINK_OK`。
- 本地完整 revision：根文档 `cf5ca1da5694219ae8abd0dd877ed9bf186553e8`，
  基仓库 `0ab2d464088b003bc704a13830e1f16af0b95c14`，
  设备仓库 `8ec009fa56a5a7eb0576b76c397bb71a3d2280fd`，
  A650 fork `725b41ce59f201643ba576781fc23934a15f863b`。

镜像内直接核对到的字符串：`assets/J120.HEIC`、`J120.png`、
`addons/NativeAdrenoMetal.primelist`、`addons/NativeAdrenoMetal.cacheexport`、
`sheng-fb-gpu=1`。官方 `scripts/check-addon-writer.py` 在本构建树 PASS，
`unixnu status --json` 与 `porting-record.py check` 均 rc=0。

## 2. 两轮读数

| | of1 | of1b |
| --- | --- | --- |
| 原始抓取 | `tmp/console-of12113301.txt` | `tmp/console-of1b2123191.txt` |
| `SUPER t=230s` | `sw=114/114/0 own=1 prep=228` | `sw=1455/1455/0 own=1 prep=2910` |
| 判据 | `desktop / witness=swap` | `desktop / witness=swap` |
| PANEL | `578/1000 luma 143` | `845/1000 luma 193` |
| `WALLPAPER` | `stage=5 status=0` | `stage=5 status=0` |
| `[addon]` | `1=5/0 2=5/0 3=5/0` | `1=5/0 2=5/0 3=5/0` |
| `GPUFRAME` | `offered 0 done 0` | `offered 0 done 0` |
| `GPUCLI` | `clients=0 askers=0` | `clients=0 askers=0` |
| `GPUROW23` | `PUBLISH self test failed` | `PUBLISH self test failed` |
| `GPUROW24` | `SELFTEST rounds 1 4` | `SELFTEST rounds 1 4` |

两轮都把 `sheng-fb-gpu=1` 写进命令行，但 `GPUFRAME offered 0` 且
`GPUCLI askers=0`，所以这不是零拷贝交接成功的证据。画面由当前仍在运行的
CPU 呈现路径产生；`NativeGOPPresenter` 只有在 GPU 交接真正建立后才移除 CPU 拷贝。

## 3. 源码定位

`package/GpuProbe.cpp:1945` 写着：

```c
if (selfTest() && gMode >= 12) metal();
```

两轮的 `GPUROW23` 都是
`PUBLISH self test failed ... rounds 1 4 ... tgt deadbeef deadbeef deadbeef deadbeef`，
`GPUROW24` 都是 `SELFTEST rounds 1 4 ... steps 36/36 r1 18/18`。
`selfTest()` 的两个 user-client round 返回值分别为 1 和 4，按
`package/GpuProbe.cpp:1625` 的 `round>=3` 判据没有通过，所以 `metal()` 不执行，
IOAcceleratorES / Metal 插件没有发布。三个 CP marker 没被写入，目标页仍是
`0xdeadbeef` 填充；这是 self-test 内的 CP 提交/完成问题，不是壁纸 overlay 或
shader cache 的问题。

官方 `porting/gpu.md` §4 要求 Metal 设备发现、客户端连接、pipeline 创建、渲染和
呈现分别验证。当前两轮不满足这一层，且 `porting.toml` 的 `gpu` 状态仍写
`device-verified`，属于待纠正的旧状态。

## 4. 壁纸

官方 PR #40 (`f477f79`) 的 iPadOS 17 Flat / J120 overlay 已生成、打包并挂载：
三个 addon mount 都到 stage 5 / status 0，`WALLPAPER stage=5 status=0`。
但两轮都没有 GPU/Metal 客户端，of1 仍出现 15 次 AegirPoster abort 和
12 次 KaleidoscopePoster fault；of1b 没有 poster 进程名，但不能据此说壁纸渲染成功。
没有像素或人眼证据，壁纸不判完成。

## 5. 帧率与 120 Hz

of1b 在 t=180 到 t=230 的 `sw` 从 892 到 1455，约 11.3 次交换/秒；
of1 同段约 1 次交换/秒。两者触控输入不同，且都没有 GPU 交接，不能用来宣称
120 Hz 桌面体验。官方显示页要求分别记录请求刷新率和测得帧时序；
`sheng-vsync=120` 只是请求值。下一轮必须带 `sheng-gpu-trace=1`、
`sheng-gpu-perf=1` 和诊断导出，在同一触控场景比较七个延迟阶段。

## 6. 边界与下一步

- self-test 失败连续出现两次，按 `PLAN_NEW.md` 的停手条件不再追加同类板轮。
- 下一步在源码里查 `ucRound()` 第 1 轮为何只返回 1：三 marker、fence、
  rptr/wptr 和 target 页各自在哪一步停住；改好后再用一次 parity 构建验证
  `GPUROW23 PUBLISH ok` 与 `GPUROW24 METAL ok`。
- 然后才重新判零拷贝交接、壁纸、pipeline 预热和帧诊断，不把 CPU 路径的
  `desktop` 判定写成 GPU 完成。
