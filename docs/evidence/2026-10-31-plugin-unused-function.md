# parity 的插件在 arm64（非 arm64e）构建下有一个 unused function

## 事实

`build-adreno-metal-plugin.py` 对 `ios` 目标用 `-target arm64-apple-ios17.0`，
`base` 里带 `-Werror`（`scripts/build-adreno-metal-plugin.py:26`）。

编译失败：

```
drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm:95:17: error: unused function
  'patchFramebufferBranch' [-Werror,-Wunused-function]
```

## 原因

`patchFramebufferBranch` 定义在 `:95`（**没有条件编译**），唯一的调用在 `:365`：

```cpp
if (!strcmp(getprogname(), "backboardd") && bootArgSet("sheng-fb-bgra=1")) gpu->mark(patchFramebufferBranch(slide));
```

而 `:365` 所在的块被 `#if defined(__arm64e__)` 包住（`:341` 开始）。**`ios` 目标编的是
arm64（不是 arm64e），所以那个块被编掉，函数就没有调用者。**

**这是上游代码的不一致：函数与它的调用没有同一个条件。**

## 为什么不在基仓库加 `-Wno-unused-function`

AGENTS.md 的架构隔离：基仓库只改挂载点。`-Wno-unused-function` 是给上游代码的缺陷打补丁，
加了就等于把上游的 bug 变成我们的默认行为，而且下次同步要再处理一次。

**同样的理由已经用过一次**：`decisions/2026-10-31-mesa-patch-blocker.md` 里「不改补丁去适配
旧钉点」。这里同理。

## 三条路

| # | 做法 | 代价 |
| --- | --- | --- |
| 1 | 报上游：函数与调用应有同一个 `#if` | 一次 issue |
| 2 | 等上游修 | 时间不定 |
| 3 | 本地给插件加 `-Wno-unused-function` | 越界改动，且与「与上游同源」冲突 |

## 现在能做的

**插件重建不是本轮必需项**（`docs/evidence/2026-10-31-stale-metal-addon.md` 已写明
`GPUROW23`/`GPUROW24` 证明旧插件能加载能注册）。这一条只影响「插件能否用 parity 的源重建」。

## 已确立的进展（与本条无关，但同一次尝试里拿到的）

| 步骤 | 结果 |
| --- | --- |
| mesa 的 11 个补丁按顺序应用 | **全部 `rc=0`**；`validate_recorded_patches` → `VALIDATED` |
| Turnip | **PASS**，17,881,712 字节 |
| bridge（metal2vulkan + adreno-metal-bridge） | **Finished**，3m 46s，`libadreno_metal_bridge.a` 11:00 |
| 插件 | **停在这条** |

**所以 `docs/evidence/2026-10-31-mesa-patch-mismatch.md` 的「补丁与钉点不符」是错的
（该文件已有更正段）。真正的阻塞在插件这一步，且是上游代码的 `#if` 不一致。**
