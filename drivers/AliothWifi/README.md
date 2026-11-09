# AliothWifi

QCA6390 (PCI 17cb:1101) Wi-Fi for alioth: Linux ath11k PCI/MHI/QMI built against UniXNU's shared
Wi-Fi ABI (`Ath12kWifi`'s `nw_*` runtime), with this board's PCIe, SMMU, DMA, MSI and firmware.

## State

Imported from `qiumochen/UniXNU-zizhan` (`drivers/PongWifi`), the same structure on QCA6490 /
SM8475 that reached scan, WPA2, DHCP and HTTPS on the device (its record g275–g337). Only the
ath11k glue is here so far; the board platform, the network layer and the build recipe are the
next steps, and nothing in this directory is built into the image yet.

| File | State |
| --- | --- |
| `ath11k/AliothAth11k.c` | The ath11k ↔ `nw_wifi` glue, adapted to QCA6390 (device id 0x1101). Refers to `alioth_ath11k_pci_init/_exit`, which the platform file will provide. |
| `ath11k/compat/` | The shim headers the Linux sources build under (`PongCompat.h`, `PongWait.h`, `linux/*`). Names kept as imported; they are found by include path, not by name. |
| *(next)* platform | Cold start for this board: reuse `package/alioth_pcie.c`'s trained link, BAR and MSI; own the endpoint's SMMU context, the DMA window and the WLAN rails. Pong's PMIC/RSC/PHY constants do not transfer. |
| *(next)* net | `PongWifiNet.cpp` adapted from Darwin 24 to this image's Darwin 23 Apple80211 ABI. |
| *(next)* build | `scripts/build-alioth-ath11k.py`, from `build-pong-ath11k.py`: compile Linux ath11k from the pinned `v7.0.14` tree against the `Ath12kWifi` shim. |

## Firmware

`proprietary-files.txt` pins `ath11k/QCA6390/hw2.0/{amss.bin,m3.bin,board-2.bin}` (linux-firmware,
SHA-256 recorded); `scripts/extract-files.py` fills `.cache/firmware` and `--check` verifies it.
The driver reads them through the addon at `/private/var/ngpu/firmware/qca6390/`, which is the
existing `NGPUADD2` channel on this OS profile (`payload.runtime` is iPadOS 27 only per
`docs/zh/porting/packaging.md`).

The generic board file is the first boot set. The phone's own BDF lives in a vendor partition and
is a later substitution; the record states this so a scan result is not attributed to board data
that was never used.

## License

Files keep their `SPDX-License-Identifier: BSD-2-Clause-Patent` headers from UniXNU-zizhan. The
Linux ath11k sources they compile are GPL-2.0 and stay in the build cache, never in this
repository.
