# `GPUROW0` 报 `m7`，镜像里是 `pipa-gpu=12`

2026-10-31 09:07 的 `zap3` 轮次（镜像 `tmp/boot-zap3.img`，`sheng-build-tag=zap3`，
191,264,768 字节）读到：

```
GPUROW0 GPU m7 gcc=ok cc=ok smmu=ok gpu=skip io=0 cx1 clocks1 ahb0
```

而同一镜像里嵌的命令行是：

```
… sheng-touch-hid=1 pipa-gpu=12 sheng-gpu-addon=1 … sheng-build-tag=zap3
```

（`strings tmp/boot-zap3.img | grep -oE 'args = ".*"'`，命令行 1011 字节，
`pipa-gpu` 在第 829 字节。）

## 已经排除的

| 假设 | 怎么排除的 |
| --- | --- |
| 命令行被截断 | 1011 字节 < `BOOT_LINE_LENGTH` 1024（`pexpert/arm64/boot.h:25`）；`DarwinBoot.c:617-625` 的拒绝条件是 `Index >= 1023` 或字节 > 0x7f，两者都不成立 |
| `pipa-gpu` 出现两次 | `grep -o 'pipa-gpu'` = 1 |
| 解析匹配到子串 | `PE_parse_boot_argn_internal` 要求 `i == strlen(arg_string)`（`bootargs.c:105-106`） |
| 分隔符把 `-` 当界 | `isargsep` 只认空格、`\t`、`\0`（`bootargs.c:37-43`） |
| 有两个 `native_gpu_probe` 实现 | `nm -g` 全归档只有一个 `T _native_gpu_probe`，在 `libAdreno740.a` |
| `GpuProbe.o` 编了两遍 | `build/adreno740/` 下只有一份 `GpuProbe.o` |
| `gMode > 12` 被截到 12 | 12 不触发截断；而且截断只会得到 12，不会得到 7 |
| 板子跑的是别的镜像 | 持久日志 `build/sheng-logs/zap3-00000002d39cb2d3/header.txt` 的 `tag` 是 `zap3` |

## 还剩什么

`native_gpu_probe()`（`GpuProbe.cpp:1557-1563`）：

```cpp
unsigned mode = 0;
if (gMode || !PE_parse_boot_argn("pipa-gpu", &mode, sizeof mode) || !mode) return;
gMode = mode > 12 ? 12 : mode;
```

`mode` 是 `unsigned`，`sizeof mode` 是 4。**如果 `PE_parse_boot_argn` 返回 false，
函数直接 return，探针一行都不会打印。** 探针打印了 15 行，所以解析返回了 true 且
`mode != 0`。

**唯一没有排除的可能：`7` 不是从命令行读的，而是 `r.mode` 在 `gpb_run` 之前被别处写过。**
`gpb_run` 的第一句是 `__builtin_memset(r, 0, sizeof *r); r->mode = mode;`（`GpuPowerBoard.h:132-134`），
所以 `r.mode` 只能是入参。入参是 `gMode`（`GpuProbe.cpp:1525`）。

## 下一轮怎么判（一条，不需要改代码）

在 `native_gpu_probe()` 里，`gMode = …` 之后加一行打印：

```cpp
snprintf(native_gpu_rows[kRowSummary], sizeof native_gpu_rows[0],
         "GPU m%u arg=%u parsed=%d", gMode, mode, (int)parsed);
```

`rowsBasic()` 在 `gpb_run` 之后会**覆盖** `kRowSummary`，所以这一行要写到 `kRowContext`
（13）——它在冷启动之前不会被覆盖，而 `GPUROW13` 是探针打印的第三行。

**判据**：`GPUROW13` 里出现 `arg=12 parsed=1` 而 `GPUROW0` 仍是 `m7`，说明 `gMode` 被别处
改写；若 `arg=7`，说明命令行在这一轮确实不同（那就去核板子实际加载的镜像 sha256）。
