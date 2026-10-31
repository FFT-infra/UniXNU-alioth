# `GPUROW28` 的 UCHE OOB：第三次提交读到未映射的 IOVA

2026-10-31 的 `zap7`/`zap7b`/`zap7c` 三轮（同一镜像，三次一致）都停在同一个地方：

```
GPUROW25 LIVE irq115017 last200 seen900200 m1 | sub 3/2 t1019609 p0 | uc 5/3 | bo 0/0 r0 s0 | busy 999
GPUROW28 FAULTS cr0 260406 gfsr 0 syn 0/0/0 | cb0 fsr 400 syn 40c0e far 5e09e75794f5
```

## 读数本身是对的

`smmuRead(bank0 + 0x64)` 在前、`smmuRead(bank0 + 0x60)` 在后，拼成 `high<<32 | low`。
Linux 对同一个寄存器用的是 `readq_relaxed(cb_base + ARM_SMMU_CB_FAR)`
（`reference/k40-kernel/drivers/iommu/arm-smmu.c:1888`、`:2081`），`ARM_SMMU_CB_FAR = 0x60`
（`arm-smmu-regs.h:180`）。小端机上 `readq` 读 `0x60`–`0x67`，低字在 `0x60`、高字在 `0x64`。
**探针的拼法与 Linux 相同，所以 `far = 0x5e09e*` 是真实读数，不是拼错。**

## 这个地址是什么

| 量 | 值 |
| --- | --- |
| `far` | `0x5e09e75794f5` = **94.0 TiB** |
| GPU IOVA 上限（`kDeviceInfo`） | `0x1fffffffff000` = 512 TiB |
| 客户窗口 slot0 起始 | 512 MiB（`reference.md §31`） |
| slot0 窗口 | 170 MiB |

**94 TiB 在 IOVA 上限之内，但比任何客户窗口大六个数量级。** 所以它不是某个 BO 的地址，
是**一个从未被映射的 IOVA**——GPU 读到它，SMMU 报 OOB（`fsr 400` 是 UCHE 的
`UCHE_INVALID_OP` 类），提交停在那里。

## 什么时候发生

`GPUROW25` 的序列说明了：

| 阶段 | `sub` | `busy` | 含义 |
| --- | --- | --- | --- |
| 内核自测 | `2/2` | `0` | 两次提交都完成 |
| 用户态之后 | `3/2` | `999`–`1000` | **第三次提交永不完成** |

`uc 5/3`：5 个客户端、3 个成功。`GPUROW23` 是 `PUBLISH ok … rounds 4 4`——发布阶段本身是好的。

**所以故障在「Metal 的第一个真实提交」，不在发布、不在 zap、不在 GMU。**

## 与已知结论的关系

`reference.md §31` 记的是「客户端窗口」的读数口径（`peak_mib` 是绝对地址、`GPUSLOT hi` 是相对值），
以及 slot0 的 967 MiB 约束。**本条不是那个问题**：那是窗口容量，这里是提交里出现了一个
窗口之外的地址。

`reference.md §33.5` 记的是 BO 池（768 MiB，峰值 95% 满）。parity 分支把固定池去掉了
（`porting.toml` 与 `2026-10-31.md §3.5`），**所以 BO 池不是这里的约束**——但「某个 BO 的
IOVA 被算到了 94 TiB」与「池子不够」是两类问题，不能混。

## 下一轮（离线先做，不需要板子）

1. **94 TiB 从哪来**：`0x5e09e*` 与 `kDeviceInfo` 的哪个字段有关？先把 `NativeAdrenoUserClient`
   的 IOVA 分配点列出来（`windowStart_`、`windowPeak_`、每次映射的 iova），看有没有一处会把
   地址加到一个过大的基址上。
2. **`fsr 400` 与 `syn 40c0e`**：`40c0e` 的低位是 `0x0e`（UCHE 的 OOB 子类），高位 `40c0`
   可能是 cache 操作类型。对照 `a6xx.xml.h` 的 `UCHE_INVALID_OP` 位域。
3. **`GPUROW23` 的 `rounds 4 4`**：四次提交都成功。**故障只出现在第五次**（用户态的第一笔）。
   所以先查「用户态提交与内核提交在 IB 构造上的差别」，而不是查 SMMU 配置。

**判据**：在 `NativeAdrenoUserClient` 的提交路径里加一行打印，记录每次提交的 IB 首地址与
长度，看第三次的 IB 里有没有 94 TiB 的地址。
