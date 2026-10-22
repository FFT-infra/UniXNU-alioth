// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op definitions of the probe hooks the patched XNU references while alioth_defconfig leaves their component out.
// Each hook runs only on its boot argument, which board.toml does not pass; the kernel link needs the symbol anyway.

// offline-component-load-probes.patch, bsd_init: called on offline-legacy-register=1 (NativeLegacyDisplay).
void native_legacy_display_probe(void);
void native_legacy_display_probe(void)
{
}
