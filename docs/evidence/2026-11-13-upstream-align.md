# 2026-11-13 upstream alignment build

This record covers the source rebase and the build. It does not claim a board run.

## Revisions

| Repository | Revision |
| --- | --- |
| Upstream base `UniXNU/UniXNU` | `2366e33e4934696424bb9f5aa7598df925fbc0c1` |
| Local base `src/UniXNU`, branch `align-main-20261113` | `fe14b996a4fd6e44a1b19f452ea8d2e48fcc6099` |
| Local device `src/UniXNU-alioth` | `b2962ead339e9de322be500974eb9edb5e1e8b08` |
| Device AdrenoMetal, branch `fft` | `3b84be5e81251ecba8c32b8928899f3c56a83dd0` |

Component gitlinks in the aligned base:

| Component | Revision |
| --- | --- |
| AdrenoMetal | `73d582fd4d5772b1f5fa83581a25077a16f2b6cd` |
| Adreno740 | `54b41b8f59e90a167f6f75160739c5f825945fbc` |
| GopFramebuffer | `b0d812d578efa2acb005b66f73168891719af09a` |
| ArmCpu | `d90e457b8392623853ec85c860feef546c767621` |
| AppleKeyStoreState | `6ff4f11d8fdf853351f8d98e1bbe1373fb8da2e6` |
| DarwinBoot | `5243c68ec332a227fe3680cd5da6d5a0e127035c` |
| SoftwareAES | `da07dd5c7d62bbe960f5bd4fe7789574d3a49d8c` |
| ArmDTPlatform | `e8bd8d5de395c6cbc00d762c1bef57dbd15a448d` |
| XnuPrelink | `440589b0c31625f17edf363e24b3c82c51c70bb9` |
| RamBlock | `4be2d7eb05ef5a98126c91e0302c149140c0e3e1` |

## Deliberate exceptions

- **RamBlock stays at `4be2d7e`.** Upstream `737f6da` calls `ml_static_mfree_prepare`
  (a macOS 27 kernel API) and `native_ufs_store_write_cached` (not present in this board's
  NativeUfs checkout). The iPadOS 17 kernel link fails on both symbols. The catalog pin is
  already `4be2d7e`.
- **The device ArmCpu branch is not rebased.** It carries the alioth SMP and GIC diagnostics
  on top of `93a5030`; the base gitlink moved to `d90e457`, but this board still resolves the
  device branch.

## Build

```sh
# Base: src/UniXNU
./unixnu defconfig alioth_defconfig -D ../UniXNU-alioth
./unixnu tree -D ../UniXNU-alioth
. ../../tmp/kbuild-env.sh
./unixnu build kernel -D ../UniXNU-alioth

# Metal inputs: src/UniXNU
python3 scripts/build-adreno-metal-bridge.py --target ios
python3 scripts/build-turnip-darwin.py --target ios

# Plugin and addon: src/UniXNU-alioth
python3 scripts/build-adreno-metal-plugin.py --target ios
python3 ../UniXNU/scripts/make-owner-trust-bundle.py
WALLPAPER=tmp/addon-in36-assets/wallpaper \
  PRIME_LIST=src/UniXNU/build/prime-list.txt \
  scripts/make-gpu-addon-alioth.sh tmp/gpuaddon-align3.gpuaddon

# Payload and image: workspace
TAG=align1 ADDON=gpuaddon-align3.gpuaddon src/UniXNU-alioth/scripts/build-round.sh
```

## Outputs

| Item | Value |
| --- | --- |
| Kernel | 126,455,808 bytes, sha256 `69092d9fd6a18c3328108dcad6413b75b1a1845efe60ccc755b222c0d6069520` |
| Boot image | 191,277,056 bytes, sha256 `6a0b77e8af95ffdee56d9da1e0a10c6b28c72057c527062161d06c5d015c27e0` |
| Kernel in payload | `457ae2e31f41b7ff5044e436295a7b6325ea034bd3141d66056f6e8d32710a73`, verified by `check-payload-kernel.py` |
| GPU addon | 24,235,104 bytes, sha256 `92694e151d048300818db3bd6c8e7df29e03516fa42301a40985afc8ed5b9a80` |
| Owner trust bundle | sha256 `f958efd82ce025f44b6dfc543911f64ccff1454859f287049b545475c6799c10` |
| Plugin CDHash | `acc3eeee0cba872738c582b036434032716f1a31` |

The addon carries the rebuilt plugin and Turnip. The plugin carries the upstream `SA` row,
`PRb` row, `metal-flow=`, `wallpaper-freeze=1`, and the embedded `__primelist` section
(12,433 bytes).

## Boot arguments

The aligned components parse the names without the `sheng-` prefix. The board now passes:

```
vsync=120 fb-bgra=1 fb-gpu=1 pmgr=2
```

`fb-gpu=1` is also accepted by the board's `LegacyDisplayProbe`; `sheng-fb-gpu` remains a
fallback for old images. Other board-private probes keep their `sheng-` names.

## Checks

| Check | Result |
| --- | --- |
| `unixnu status --json -D ../UniXNU-alioth` | rc=0 |
| `porting-record.py check --device ../UniXNU-alioth` | rc=0, 15 areas |
| `check-addon-writer.py` | PASS |
| `check-payload-kernel.py` | rc=0 |
| `check-alioth.py` with `UNIXNU_UEFI=../uefi-alioth` | rc=0 |

No `fastboot boot` was run for this record.
