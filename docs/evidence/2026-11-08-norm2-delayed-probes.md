# norm2: moving the store probes off the first exec does not change the stall

## Round

- Image: `tmp/boot-norm2.img`, sha256 `3c7a004281e74a315e3574076858c4df8934f96f26d68fc84b9bdd45bfe4a7b9`.
- Source: `d37fbcc` (both store probes behind 200 execs), the same kernel tree as norm1 otherwise.
- Kernel: built `Sun Nov 8 21:29:05 CST 2026`, 125,861,888 bytes (the same kernel as the desktop
  rounds `wl1600071` and `w41643142` and the failing rounds `w7`/`w8`/`w9`/`w10`).
- Console: `tmp/console-norm22352251.txt`, sha256
  `116fdcae86fc74484df5aa3e6e051504e858461b1654a9e809c4d9de9cff540b`.
- One round: `sh tmp/rounds-count2.sh tmp/boot-norm2.img norm2 1 300`;
  `norm22352251: RESULT t=290 own=0 sw=0 data=2 frame=0 askers=0 -> no-desktop`.

## What the delayed probes did

No `[wallpaper db]` or `[wallpaper list]` row appears anywhere in the console: neither probe ran.
`DATAMOUNT stage=4 status=0 ro=0 store=2` and `[addon] 1=5/0 2=5/0 3=5/0` are the same as norm1.

## The stall is unchanged and is not caused by the probes

```
CORES t=20s c0=13826 c1=225 c2=20 c3=65 c4=13043 c5=3317 c6=902 c7=230 deaf=00
CORES t=30s c0=20466 c1=225 c2=20 c3=65 c4=13043 c5=3812 c6=1152 c7=622 deaf=1e
CORES t=290s c4=13043/770 (unchanged)                                   deaf=1e
GPUTH t=30s st=9 ev=fffffff00b893f24 nm=gpu_probe
KOBJ  t=30s tid=0 locks=15283 (unchanged through t=290)
PROC  t=290s a=9 p=9 s=1     SUPER t=290s own=0 sw=0/0/0
```

c4 stops at the same counter value and never moves again; c1–c3 are idle with no deadline. The
boot stops before any userland client, exactly as norm1. So the first-exec store probe is not the
trigger, and `d37fbcc` is neither the cause nor the cure of this stall.

## The stall is not a regression from the evening commits

`wl1600071` (desktop, 16:04), `w41643142` (desktop, 16:47) and `w7`/`w8`/`w9`/`w10` (all
`own=0`, 17:14–19:00) all report the same kernel build time, `Sun Nov 8 15:07:52`. The same binary
reaches the desktop and stalls. The evening changes (wallpaper grants, the store probes, the GPU
spin waits) are therefore not what turned a working image into a failing one; the failure is a
nondeterministic kernel race that the same image exposes at different rates.

## Where the core is

The newest interrupt-mask site for c4 is `_lck_rw_clear_promotion+0x20`, caller
`_lck_rw_done+0x28c`, thread `0x109`:

```
GICRM t=30s c4 n=54052/40721 ctl=1 rem=-3691766859
  fffffff00b8b7390<fffffff00b8b6ad4/109   lck_rw_clear_promotion+0x20 <- lck_rw_done+0x28c
  fffffff00b916d5c<fffffff00b8d69bc/109   waitq_wakeup64_all+0xa0 <- thread_wakeup_prim+0x48
```

The disassembly of `_lck_rw_clear_promotion` shows the masked section it entered:

```
fffffff00b8b738c  bl  _ml_set_interrupts_enabled(0)   ; mask interrupts
fffffff00b8b7390  mov x20, x0                          ; <- the recorded site
fffffff00b8b7394  add x0, x19, #0xa8                   ; x19 = current thread
fffffff00b8b7398  bl  _lck_spin_lock                   ; the thread's own sched_lock
```

`_hw_lock_lock` (the sched_lock path) reads the lock with `ldaxr` and, when it is held, executes
`wfe` before `hw_lock_lock_contended`; `hw_lock_unlock` stores zero and sends no event. The `wfe`
therefore waits for the generic timer's event stream (`CNTKCTL_EL1.EVNTEN`), and that `wfe` has
no timeout of its own: the lock-panic timeout lives only in the contended loop after it. A core
that reaches that `wfe` without the event stream never reaches the timeout and never takes the
timer or an IPI, which is the observed state.

## Next reading (lkl1)

One image reads, for each deaf core: the thread's own `sched_lock` word and, when it is a thread
pointer, that holder's tid/state/priority/name/core/continuation/event and a stack scan; the
`lock_panic_timeout` and `spinlock_timeout_panic` values; and `CNTKCTL_EL1` for every core as read
in `initCPU` plus this core's live value. Those three answers split free-lock / self-lock /
held-by-another, and event-stream-off / event-stream-on.

## Boundary

Desktop, wallpaper, widgets and Wi-Fi remain unverified in this round. No completion level is
promoted.
