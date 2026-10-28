# GPU SMMU: the free-slot test, and why the board fixes it instead of the kit

2026-10-29, alioth (SM8250). Mode 7 of the GPU probe, `pipa-gpu=7`.

## Symptom

The GMU context could not be built. `GPUROW13` read `routes 0` and the Attach step returned
`e00002ca`, which is `NativeAdrenoSmmuContext::attach` folding every non-success into
`kIOReturnIOError`. The context programming itself had already succeeded — phase 5, 13 writes,
read back consistent — so the failure was in stream routing, not in the context bank.

## Cause

`kits/ArmSmmu4K/ArmSmmu4KStreams.c:47` decided a stream slot was free only when its S2CR read
exactly `0x00020000`:

```c
} else if((s2cr&~valid_s2cr(r->id0))!=FAULT || ...) return ArmSmmu4KBusy;
```

`valid_s2cr()` returns `1<<10` only when the part reports EXIDS, and this GPU SMMU does not
(`id0 = 0x4c017e06`, bit 8 clear), so the mask is zero and the comparison is against the whole
word. The part's unused slots read `0x000203ff`: type FAULT with `CBNDX = 0xff` left in the low
byte. Every slot the kit looked at therefore came back busy, and no route was ever written.

With type FAULT the hardware ignores the rest of the word, so the strict comparison is stricter
than the architecture requires. The same conclusion is recorded upstream in a fork for the same
SoC: `xVanTuring/ArmSmmu4K`, branch `pipa`, commit `9fbc33a`, measured on the Xiaomi Pad 6.

## Fix, on the board

Before each attach the probe rewrites the S2CR of the slots it is about to use — GMU 2 and 3,
GPU 0, 1, 4 and 5 — to the canonical FAULT word. It only touches slots whose type is already
FAULT, so a slot carrying a real route is left alone.

```c
for (unsigned i = 0; i < sizeof(kGmuSlots) / sizeof(kGmuSlots[0]); ++i) {
    volatile uint32_t *s2cr = &s[(0xc00 / 4) + kGmuSlots[i]];
    if ((*s2cr & (3u << 16)) == (2u << 16)) *s2cr = 0x00020000u;
}
```

The shared kit stays at its upstream commit. When upstream adopts the type test, this loop
becomes a no-op on the slots it touches and can be removed.

## Evidence

Same probe, same mode, the only difference being the normalisation:

| Row | Before | After |
| --- | --- | --- |
| `GPUROW13` | `routes 0`, `status e00002ca` | `routes 2`, `phase 5 wr 13 rb 1 en 1` |
| `GPUROW14` | (cold start never ran) | `phase 8 blocks 16 version 20010008 hfi 1 cm3 1` |
| `GPUROW15` | (no HFI) | `stage 8 acked c6 error 0 sent 4` |

The three rows match, field for field, the ones produced by a build that carried the same change
inside the kit, which is what makes the two approaches interchangeable for this board.

## Two routes that did not work

1. **Clearing the slot to zero.** An earlier round wrote `0` into a slot whose SMR was clear.
   That sets type TRANSLATE, not FAULT, so the slot stops looking free and the attach fails the
   same way. The value matters: `0x00020000` is the FAULT word, `0` is not.
2. **Pointing the superproject at a local kit commit.** `unixnu` runs
   `git submodule update --init --recursive` before building (`scripts/unixnu/feeds.py:208`), so
   it checks the submodule out at whatever commit the pin names. With the working tree holding a
   different commit the build stops at
   `fatal: Unable to checkout '<upstream sha>' in submodule path 'kits/ArmSmmu4K'`. A local
   `--local-module kit:...` override does not help; the checkout happens first.

## Why not a fork

The device-fork route is the documented one, and the pipa board uses it: its `.gitmodules` points
`kits/ArmSmmu4K` at `https://github.com/xVanTuring/ArmSmmu4K.git`, branch `pipa`, and the gitlink
names a commit on that branch. A feed entry would not work here — `feeds.py` resolves git sources
after the base repository's own submodules, so the base copy shadows it.

This board does not need that machinery for a six-line change, and the normalisation is verified
equivalent. If a second or third change to the kit ever becomes necessary, switch to the fork
layout then.

## Still open

Mode 7 ends at the GMU cold start. The accelerator is not published: GX power is mode 8 and the
GPU's own context with its first command is mode 9, and the probe's publish row stays empty until
one of those runs.
