# 第一次读到真实的 IB 内容：之前那些「全 0」是读取失败的默认值

## 根因

`NativeAdrenoVm.h:30` 把 `native_adreno_dram_read32` 定义成**板级钩子**：

```c
/* Board hook for sampleWord(): reads one aligned word of physical memory through a fresh
 * uncached mapping; 0 on success. */
extern "C" int (*native_adreno_dram_read32)(uint64_t physical,uint32_t *value);
```

`NativeAdrenoVm.cpp:557` 的 `dramWord` 第一句是 `if(!native_adreno_dram_read32)return false;`。
`peekWords`（`:566`）在入口 `out[k]=0`，找不到 CPU 视图就走 `dramWord`，失败即返回
`kIOReturnUnsupported`，**`out` 保持入口的全 0**。`sampleWord`（`:488`）同理，初值是 `~0u`。

**全仓库没有任何地方给这个指针赋值**：`BootProgressStubs.c:317` 只把它定义成 `= 0`。
所以 2026-10-31 之前每一轮的 `[ib]` 28 个字全 0、`[coherence] cached=before=after=ffffffff`，
**都是读取失败的默认值，不是内存内容。**

## 修法

`GpuProbe.cpp` 的 `native_gpu_probe()` 在起线程前赋值，实现按头文件的约定用
`IODeviceMemory::withRange(page,0x1000)` + `kIOMapInhibitCache` 读一个物理字
（一次一页，页变了重建）。提交 `b16e89e`。

## 修好之后读到的（`ibread3`，`build/sheng-logs/ibread3-00000002041afeb6`）

```
[ib] fence=5 iova=020004000 bytes=48 base=020004000 off=0 wc-distrusted
I00 703d8003 20005000 00000000 0badc0de 703d8003 20005004 00000000 feed0001
I08 703d8003 20005008 00000000 600d0003 00000000 00000000 00000000 00000000
[coherence] samples=1 cached=ffffffff before=703d8003 after=703d8003 distrust=1
```

- `0x703d8003` = `CP_MEM_WRITE`（opcode `0x3d`），`0badc0de`/`feed0001`/`600d0003` 是内核自测
  写下的标记——**真数据，不再是 0**；
- `before == after == 703d8003` 说明写合并修正之后内核与 DRAM 一致；
- `wc-distrusted` / `distrust=1`：这一次的审计发现了脏行并切到保守模式。

## 作废的结论

`docs/evidence/2026-10-31-ib-log.md` 里「Metal 提交了一个内容为 0 的 736 字节 IB」**不成立**：
那个 736 字节的读取本身失败，读到的是默认值。**这一条要从 `porting.toml` 的 `gpu` 区 findings
里删掉或改写**（见本轮后续提交）。

## 这一轮还带出的两件事（未解）

1. **故障点前移**：`GPUROW23 PUBLISH self test failed submit 0 … uc 16/19 first e00002d6 at 15`
   —— 内核自测的发布阶段就有 3 个客户端拿不到设备（`e00002d6` = `kIOReturnNotReady`），
   而之前几轮是 `uc 36/36` 全成功。
2. **`syn` 从 `40c0e` 变成 `10c0e`**：`far` 仍是 `5e09e75794f5`，但 FSYNR0 的高位变了。
   两者差 `0x3000`，那是 `FSYNR0` 里的操作类型位段，说明**这次的访问类型不同**。

**这两条都要在下一轮单独查，不能与「空 IB」混为一谈。**
