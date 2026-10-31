# parity 的 `turnip-gmem-mutable-swap.patch` 与它自己记录的 Mesa 钉点不匹配

## 事实

| 项 | 值 |
| --- | --- |
| `scripts/data/adreno-compiler-inputs.json` 的 mesa 钉点 | `85fda3b36405bcaea8bffc64aa4f2651dc90770d` |
| 本地 `.cache/mesa-work` 的 HEAD | `85fda3b36405bcaea8bffc64aa4f2651dc90770d`（一致） |
| 仓库 | `https://gitlab.freedesktop.org/mesa/mesa`（一致） |
| 工作区 | 干净（`git status --short` 为空） |
| `turnip-gmem-mutable-swap.patch` | 539 行，与 parity 的版本逐行相同 |

**在这个钉点上，补丁打不上，三个文件同时失败：**

```
error: patch failed: src/freedreno/vulkan/tu_clear_blit.h:103
error: patch failed: src/freedreno/vulkan/tu_clear_blit.cc:206
error: patch failed: src/freedreno/fdl/fd6_view.cc:537
```

## 原因：补丁引用了钉点里不存在的函数

`turnip-gmem-mutable-swap.patch:273-275` 期望的上下文是：

```
@@ -103,6 +103,12 @@ tu_choose_gmem_layout(struct tu_cmd_buffer *cmd);
 bool
 tu_attachment_unpadded(struct tu_cmd_buffer *cmd, const struct tu_image_view *iview);
```

而钉点 `85fda3b` 的 `tu_clear_blit.h` 里：

```
grep -c tu_attachment_unpadded src/freedreno/vulkan/tu_clear_blit.h   → 0
grep -c tu_attachment_unpadded patches/mesa/turnip-gmem-mutable-swap.patch → 2
```

**`tu_attachment_unpadded` 在钉点里根本不存在。** 补丁是针对一个**更靠后的** mesa 写的
（那是 `tu_attachment_unpadded` 被引入之后的版本）。

## 排除的操作因素

| 假设 | 怎么排除的 |
| --- | --- |
| 工作区脏 | `git status --short` 为空；之前 19 个改动全部来自补丁（用补丁的 `diff --git` 路径集合逐条核过，`EXTRA = []`） |
| 补丁顺序错 | `adreno-compiler-inputs.json` 的 11 条与 parity 的 11 条**逐条相同、顺序相同** |
| 补丁文件被本地改过 | `wc -l` 与 `git show upstream/…` 的版本相同（539/539、149/149、1939/1939） |
| 钉点取错 | 完整 40 位哈希与 `origin` 都一致 |
| 之前构建的残留 | `git checkout -- .` + 删掉三个未跟踪文件后重跑，仍然同样失败；失败后 `git status` 仍是 0（apply 会回滚） |

## 影响

**Metal 插件无法从 parity 的源重建。** `build-turnip-darwin.py` 在补丁阶段就停，
所以 `build-adreno-metal-bridge.py` 与 `build-adreno-metal-plugin.py` 都跑不到。

`docs/evidence/2026-10-31-stale-metal-addon.md` 记的是「这一轮用的是 10-30 的旧插件」——
**原因是这条阻塞，不是疏忽。**

## 下一步（三条，按代价从低到高）

1. **核上游是否已经修了**：`git log --oneline -5 upstream/feat/ipados27-gpu-parity -- patches/mesa/turnip-gmem-mutable-swap.patch`
   并看 `adreno-compiler-inputs.json` 的 mesa 钉点有没有更新过。**如果上游把钉点往前推了，
   这条就自动消失。**
2. **报告给上游**：这份补丁与它记录的钉点不匹配，是 parity 分支自身的数据问题
   （`turnip-gmem-mutable-swap` 是 parity 新增的两份之一）。
3. **本地的临时解法**：把 mesa 钉点换成补丁实际针对的版本（需要找到那个版本）。
   **不要**改补丁去适配旧钉点——那是把上游的改动改成我们的私货，下次同步会再冲突一次。

## 现在能做的

**不重建插件也能继续**：`GPUROW23`/`GPUROW24` 已经证明旧插件能加载、能注册，
`GPUROW28` 的 UCHE 故障因此**无法归因到「parity 的插件」**（见
`docs/evidence/2026-10-31-uche-fault.md`）。这一条要在插件重建之后才能说。
