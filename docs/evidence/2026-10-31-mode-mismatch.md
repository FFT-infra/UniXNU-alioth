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

---

## 第二轮：`m12` 与 `m7` 是两种真实结果，不是读数噪声

| 镜像 | 命令行 | 运行时 `GPUROW0` | 最远行 |
| --- | --- | --- | --- |
| `a6xx2` | `pipa-gpu=12` | **m12** | 20 / **28** |
| `zap3` | `pipa-gpu=12` | m7 | 15 |
| `zap4` | `pipa-gpu=12` | m7 | 15 |
| `zap5` | `pipa-gpu=12` | **m12** | **28** |

`zap5` 这一轮：

```
GPUROW0 GPU m12 gcc=ok cc=ok smmu=ok gpu=skip io=0 cx1 clocks1 ahb0
GPUROW17 GX on perf 7 phase 4 vote 0 oob 2 gx 000000c0>00000000 gdscr f8222800 rbbm 00000001 gbif halt 0/3
GPUROW28 FAULTS cr0 260406 gfsr 0 syn 0/0/0 | cb0 fsr 400 syn 40c0e far 5e09e55794f5
```

`phase 4` 说明 `A650GmuPowerGx` 走完了（HFI 投票返回、OOB 确认收到）。

**所以：`m12` 的轮次一定走到底，`m7` 的轮次一定停在 15。** 这两个不是随机的：
同一个命令行、不同镜像，跑出两种模式。`m12` 是正确值，`m7` 是错的。

### 排除记录（第二轮新增）

| 假设 | 结论 |
| --- | --- |
| `pipa-gpu` 字符串在不同镜像里不同 | 四个镜像全是 `=12`（`strings` 逐个核过） |
| 镜像内容不同导致行为不同 | 但 `a6xx2`（旧）与 `zap5`（新）都是 m12，`zap3`/`zap4`（新）是 m7——**与新旧无关** |
| `BootProgress.c` 的扫描列表加 16 影响了 | `zap3`/`zap4` 没加 16 时也是 m7；`zap5` 加了 16 是 m12。**但 `a6xx2` 没加 16 也是 m12**，所以不是它 |

### 剩下最强的假设

`native_gpu_probe()` 是 `if (gMode || !parsed || !mode) return;`。若第一次调用时
`PE_parse_boot_argn` 失败（`gMode` 保持 0），函数返回；**如果它被调用第二次**，第二次
可能在 boot args 已被别的东西改写之后，读到别的值。

`native_gpu_probe()` 只有一个调用者（`LegacyDisplayProbe.cpp:945`），在
`native_legacy_display_late_probe()` 里，**那本身可能被调用多次**（late probe 每轮
display 服务启动时跑）。查 `native_legacy_display_late_probe` 的调用次数。

### 下一轮（板子断电后）

1. 先数 `native_legacy_display_late_probe` 被调几次：console 里 `BAND` 行（
   `native_legacy_paint_reached_band` 写的第一行）出现几次。
2. 若多次 → `gMode` 的「只解析一次」保护有洞，改成「解析失败也置一个 sentinel」。
3. 若一次 → `PE_parse_boot_argn` 在 m7 那几轮确实读到了 7，去核板子实际加载的镜像
   sha256（`fastboot boot` 之后无法再查，只能核 `build/sheng-logs/*/header.txt` 的 tag）。
