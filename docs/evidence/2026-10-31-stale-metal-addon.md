# 这一轮所有镜像里的 Metal 插件都是 10-30 的旧产物

## 事实

| 物件 | 时间 | 大小 |
| --- | --- | --- |
| `NEW/UniXNU/build/adreno-metal-plugin/NativeAdrenoMetal.bundle/NativeAdrenoMetal` | **10-30 02:44** | 25,126,176 B |
| `tmp/gpu-parity3.gpuaddon` | **10-30 02:45** | 13,476,984 B |
| `NEW/UniXNU/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm` | 10-31 07:55 | — |

`drivers/AdrenoMetal` 的源在 07:55 被切到上游 parity 的 `7e0d3d2`（基仓库子模块，
`git submodule status` 实测）。**但插件二进制与 addon 都没有重建。**

## 影响

`tmp/build-parity.sh` 把 `--gpu-addon ../../tmp/gpu-parity3.gpuaddon` 直接交给 payload，
而 `unixnu build payload` 只校验这个文件「是 `make-gpu-addon.py` 造的」
（`scripts/unixnu/payload.py:121-123`），**不校验它里面的插件来自哪个源**。

所以 `parity`、`zap3`、`zap4`、`zap5`、`zap6`、`zap7` 这一系列镜像里的 Metal 插件，
都是 **10-30 用旧源编的那一份**。

## 这解释了 `GPUROW23`/`GPUROW24` 的一部分

`GPUROW23 PUBLISH ok … rounds 4 4` 与 `GPUROW24 METAL ok: accelerator registered
(IOGPUFamily loaded, class size 136)` 说明插件**能加载、能注册**——那是旧插件的能力。
`GPUROW25` 之后 `sub 3/2 busy 999` 的第三次提交，**是旧插件发出的**。

**所以 UCHE 那个 94 TiB 的读，现在无法归因到「parity 的插件」或「旧插件」**
（`docs/evidence/2026-10-31-uche-fault.md`）。要先重建插件，再重跑一轮，才能说。

## 要做的

1. **重建插件**（Mac，需要 `xcrun`）：

   ```bash
   cd NEW/UniXNU
   python3 scripts/build-turnip-darwin.py --target ios     # Turnip
   python3 scripts/build-adreno-metal-bridge.py --target ios
   python3 scripts/build-adreno-metal-plugin.py
   ```

   判据：`build/adreno-metal-plugin/NativeAdrenoMetal.bundle/NativeAdrenoMetal` 的 mtime
   是本次构建时刻，且 `codesign --verify --strict` 通过（脚本自己会跑）。

2. **重打 addon**：

   ```bash
   python3 scripts/make-gpu-addon.py --publish ... tmp/gpu-parity4.gpuaddon
   ```

   参数照 `tmp/build-a6xx-link.sh:53` 那一行（`--publish`、`--clients`、`--wallpaper` 等），
   把输出改成 `gpu-parity4`。

3. **改 `tmp/build-parity.sh` 指向新的 addon**，并把 addon 的 sha256 写进 `progress/`。

4. **判据（下一轮上板）**：`GPUROW23`/`GPUROW24` 仍然 ok，且 `GPUROW25` 的 `sub` 从
   `3/2` 变化——变化就是新插件的效果，不变说明 94 TiB 与插件无关。

## 为什么之前没发现

`tmp/build-parity.sh` 是我写的，它检查了 driver 与 kit 的钉点，**没有检查 addon**。
计划 §C4 只写了「不再手工 export kit 路径」，没写「addon 也要跟着源重建」。
**这是同一类错：检查了三个输入中的两个。**
