# QCA6390 (17cb:1101) board facts for alioth

Collected 2026-11-09 from the vendor tree `reference/k40-kernel` (`f3d39dae`) and mainline
`reference/repo/linux-mainline`. This is the input sheet for `drivers/AliothWifi`; every value has a
file:line behind it in the vendor tree, and the mainline equivalents agree unless stated.

## PCIe0 (RC0)

| Resource | Vendor `kona-pcie.dtsi` | Notes |
| --- | --- | --- |
| `parf` | `0x01c00000` + `0x3000` | same in mainline |
| `phy` | `0x01c06000` + `0x1000` | vendor owns it inside `pci-msm`; mainline has a QMP node |
| `dm_core` (DBI) | `0x60000000` + `0xf1d` | same |
| `elbi` | `0x60000f20` + `0xa8` | same |
| `iatu` | `0x60001000` + `0x1000` | same |
| `conf` | `0x60100000` + `0x100000` | same |

- Mainline additionally maps `mhi` at `0x01c03000 + 0x1000` (absent in the vendor map).
- Outbound windows: IO `0x60200000 + 0x100000`, MEM `0x60300000 + 0x3d00000`
  (`kona-pcie.dtsi:18-19`). BAR0 (1 MB) is placed at `0x60300000`; the endpoint is bus 1 dev 0 fn 0
  (requester ID `0x100`).
- Clocks (11): `pcie_0_pipe_clk`, `pcie_0_ref_clk_src` = **RPMH_CXO_CLK 19.2 MHz**, `pcie_0_aux_clk`,
  `pcie_0_cfg_ahb_clk`, `pcie_0_mstr_axi_clk`, `pcie_0_slv_axi_clk`, `pcie_0_ldo`
  (= `GCC_PCIE_WIFI_CLKREF_EN`), `pcie_0_slv_q2a_axi_clk`, `pcie_tbu_clk`,
  `pcie_phy_refgen_clk` (100 MHz), `pcie_ddrss_sf_tbu_clk`.
- Resets: `GCC_PCIE_0_BCR`, `GCC_PCIE_0_PHY_BCR`.
- GDSC: `pcie_0_gdsc` at `0x16b004` (`kona.dtsi:2576-2581`), modelled as a regulator by the vendor.
- Rails: `vreg-1p8` = pm8150_l9 1.2 V, `vreg-0p9` = pm8150_l5 0.88 V, `vreg-cx` = `cx.lvl` (MAX).
- PHY: `qcom,pcie-phy-ver = 1102`, status at `0x814` bit 6, power-down at `0x840`, a 99-triple
  `qcom,phy-sequence` (`kona-pcie.dtsi:107-210`); the vendor replays all of it.
- `qcom,ep-latency = <10>` (10 ms), `qcom,use-19p2mhz-aux-clk`, no L0s, L1.2 th 2/70,
  `qcom,slv-addr-space-size = 0x4000000`, `qcom,boot-option = 1` (no probe enumeration; the WLAN
  driver enumerates).

## GPIO (TLMM NORTH tile, base + 0x0f900000, 0x1000 stride)

| Signal | GPIO | Level |
| --- | --- | --- |
| WLAN_EN (`wlan-en-gpio`) | 20 | active high, 1000 us hold after driving high |
| BT_EN | 21 | — |
| SW_CTRL | 124 | — |
| PERST# (`perst-gpio` on pcie0) | 79 | **active low**: drive 0 to assert, 1 to release, ~1 ms |
| CLKREQ | 80 | `pci_e0` function, pull-up |
| WAKE | 81 | pull-up |

`cnss2` enforces at least 100 ms between BT_EN off and WLAN_EN on for QCA6390/QCA6490
(`vendor cnss2/power.c:794-830`).

## SMMU

- `apps_smmu` at `0x15000000` (+ `0x15182000` TCU), `#iommu-cells = 2`.
- `pcie0` carries only an `iommu-map`: requester `0x0` → SID `0x1c00` (RC), requester `0x100` →
  SID `0x1c01` (endpoint). `qcom,smmu-sid-base = <0x1c00>`.
- The WLAN endpoint's IOMMU group (`kona.dtsi:5174-5189`): IOVA pool `0xa0000000 + 0x10000000`,
  `qcom,iommu-dma = "fastmap"`, coherent page tables, non-fatal faults, and it also uses the IPA
  window `0xb0000000 + 0x10000` (`smmu_iova_ipa`).
- Shared WLAN DMA pool `cnss_wlan_mem`: 20 MB, 4 MB aligned (`kona.dtsi:578-585`).

## WLAN rails (cnss2 supplies)

| Supply | Alioth regulator | Resource | Config (uV) |
| --- | --- | --- | --- |
| `vdd-wlan-aon` | pm8150_s6 | `smpa6` | 950000 |
| `vdd-wlan-dig` | pm8150_s6 (overridden from pm8009_s2) | `smpa6` | 950000 |
| `vdd-wlan-io` | pm8150_s4 | `smpa4` | 1800000 |
| `vdd-wlan-rfa1` | pm8150_s5 | `smpa5` | 1900000 |
| `vdd-wlan-rfa2` | pm8150a_s8 | `smpc8` | 1350000 |

The vendor WLAN node has no clocks; the reference clock is PCIe0's XO.

## Firmware

- Vendor names are built relative to the firmware-root: `qca6390/m3.bin`, `qca6390/amss20.bin`
  (QCA6390 always uses the v2 AMSS; device major must be ≥ 2), BDF `qca6390/bd_k11a*.elf`
  (or `bdwlan.*` when `board_id < 0xFF`), `regdb.bin`.
- Physically the blobs are in the **modem / firmware_mnt partition** under `image/qca6390/`
  (recorded device pull); `/vendor/firmware/qca6390` is the Android search-path alias.
- Mainline ath11k instead wants `ath11k/QCA6390/hw2.0/{firmware-2.bin,amss.bin,m3.bin,board-2.bin,
  board.bin,caldata.bin,regdb.bin}`; `board-2.bin` first, legacy `board.bin` fallback. This port
  starts with the mainline set (generic) and records the vendor set as the later substitution.

## What ath11k expects of the transport

- BAR0 only, 1 MB; `TCSR_SOC_HW_VERSION` at `0x0224`, major `GENMASK(11,8)` must be 2.
- MHI: channels 20 (IPCR, DMA_TO_DEVICE) and 21 (IPCR, DMA_FROM_DEVICE), 64 elements each, event
  ring 1; two event rings (32 elements IRQ 1, 256 elements IRQ 2, priority 1); BHI offsets read
  from BAR0 (`MHICFG 0x10`, `BHIOFF 0x28`, `BHIEOFF 0x2c`); SBL 512 KiB, `fbc_download`, RDDM
  0x420000; timeout 20 s.
- MSI: 32 vectors; MHI 3 at 0, CE 10 at 3, WAKE 1 at 13, DP 18 at 14; vector 0 is BHI. On this
  board the endpoint writes data 768.. to the doorbell at `0x17a10000`, which raises SPI 768
  (already proven end to end in `porting.toml`).
- BAR0 window mechanism above `0x80000`: `ATH11K_PCI_WINDOW_REG_ADDRESS 0x310c`, enable
  `0x40000000`, value in `GENMASK(24,19)`, range mask `GENMASK(18,0)`, dynamic windows.
- CEs: 9 host CEs (0..8) and 9 target pipes; service map WMI on CE3/CE2, HTT on CE4/CE1, reserved
  control on CE0/CE2 (`hw.c:1623-1799`).

## SMMU plan for the endpoint (next step)

Two stages, smallest first:

1. **Bypass for the first MHI boot.** The SMMUv2 register model this board already uses is in
   `src/NativeUfs-alioth/SmmuTakeover.h`: stream count at GR0 `0x20`, `SMR[n]` at `0x800 + 4n`,
   `S2CR[n]` at `0xc00 + 4n`, exact-match via `SMR = 0x80000000 | sid` (mask in bits 16..30).
   The WLAN endpoint is SID `0x1c01` (requester `0x100`, `iommu-map`); it needs its own free SMR
   slot with `S2CR = 0` (type 0, bypass), after which endpoint DMA is untranslated and
   `dma_alloc` hands out physical addresses. The UFS takeover scans the same arrays and picks its
   own slot, so the platform takes a lock around the scan/write and only uses a slot that is
   invalid before it writes.
2. **Translated window later.** The vendor shape is a stage-1 context with the IOVA pool
   `0xa0000000 + 0x10000000` and `qcom,iommu-dma = "fastmap"`. program it with the ArmSmmu4K kit
   (`ArmSmmu4KProgramDisabledContext`) once the bypass boot proves MHI, and keep the same arena.

DMA memory needs to be physically contiguous for the BHI image and the rings under bypass:
allocate one arena at boot with `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` (the GPU driver
uses the same call for its firmware pages) and hand out slices with their physical addresses.
