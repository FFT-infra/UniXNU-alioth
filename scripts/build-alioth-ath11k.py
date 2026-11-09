#!/usr/bin/env python3
"""Compile Linux v7.0.14 ath11k (PCI/MHI/QMI) against UniXNU's shared Wi-Fi ABI.

    ALIOTH_LINUX=<linux v7.0.14 tree> ALIOTH_SHARED=<Ath12kWifi checkout> \
        python3 scripts/build-alioth-ath11k.py --output-dir OUT [--compile-only]

Sources: the pinned Linux tree must carry drivers/net/wireless/ath/ath11k, drivers/bus/mhi/host,
net/qrtr, the QMI library and include/. `Ath12kWifi/tools/fetch-sources.sh` fetches exactly those
with a sparse checkout. The shared checkout supplies include/nw_abi.h, the libc/kernel shims and
the station runtime (src/nw_*.c).

The recipe is the one qiumochen/UniXNU-zizhan proved on QCA6490 (scripts/build-pong-ath11k.py),
adapted to this board: the PCI entry points are alioth_ath11k_pci_init/_exit, the WMI and MHI
hooks are alioth_ath11k_wmi_submitted/_mhi_failed, and the station nonce source is
AliothWifiRandom, which drivers/AliothWifi provides.

--compile-only (the default on a host without ld64.lld) stops after every object compiles; the
archive step needs `ld -r`, llvm-nm and llvm-objcopy, which the orb toolchain does not carry.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# In the assembled tree the device package lives at kits/<board>; the repository itself keeps the
# layer under drivers/AliothWifi. ALIOTH_WIFI_DIR names it when neither location applies.
WIFI = Path(os.environ.get('ALIOTH_WIFI_DIR', str(ROOT / 'drivers/AliothWifi')))
ATH = 'core hal hal_tx hal_rx wmi mac reg htc qmi dp dp_tx dp_rx ce peer dbring hw pcic fw p2p mhi pci'.split()
MHI = ('init', 'main', 'pm', 'boot')


def adapt_dispatcher_timers(source):
    """Do not retain intrusive-list successors across reentrant callbacks (zizhan's fix)."""
    old = """  u64 now = lx_now_ms();
  struct list_head *p, *n;
  for (p = g_timers.next; p != &g_timers; p = n) {
    n = p->next; struct delayed_work *d = container_of(p, struct delayed_work, work.entry);
    if (d->armed && now >= d->due && !is_running(&d->work)) {
      d->armed = 0; list_del_init(p); ran = 1; where(2, (const void *)d->work.func);
      g_running[g_nrunning++] = &d->work; d->work.func(&d->work); g_nrunning--; }
  }
"""
    new = """  u64 now = lx_now_ms();
  for (unsigned timer_budget = 256; timer_budget; --timer_budget) {
    struct delayed_work *d = 0; struct list_head *p;
    for (p = g_timers.next; p != &g_timers; p = p->next) {
      struct delayed_work *candidate = container_of(p, struct delayed_work, work.entry);
      if (candidate->armed && now >= candidate->due && !is_running(&candidate->work)) { d = candidate; break; }
    }
    if (!d) break;
    d->armed = 0; list_del_init(&d->work.entry); ran = 1; where(2, (const void *)d->work.func);
    g_running[g_nrunning++] = &d->work; d->work.func(&d->work); g_nrunning--;
  }
"""
    if source.count(old) == 1:
        return source.replace(old, new)
    # The catalog revision already carries this fix ("A callback can cancel and free another
    # timer: never retain the next list entry across a callback"); nothing to patch there.
    if 'A callback can cancel and free another timer' in source:
        return source
    raise SystemExit('shared dispatcher timer ABI differs')


def patch_ath11k(source, gen):
    for f in source.glob('*'):
        if f.suffix not in ('.c', '.h') or f.name == 'trace.h':
            continue
        (gen / f.name).unlink(missing_ok=True)
        (gen / f.name).symlink_to(f)
    traces = sorted({v for f in source.glob('*.[ch]') for v in re.findall(r'\btrace_ath11k_[a-z0-9_]+', f.read_text())})
    (gen / 'trace.h').write_text('\n'.join('#define ' + n + '(...) ' + ('0' if n.endswith('_enabled') else '((void)0)') for n in traces) + '\n')
    for name in ('core.h', 'dp_rx.c'):
        content = (source / name).read_text().replace('unsigned long jiffies;', 'unsigned long bp_jiffies;').replace('bp_stats->jiffies', 'bp_stats->bp_jiffies')
        (gen / name).unlink()
        (gen / name).write_text(content)
    pc = (source / 'pci.c').read_text()
    for old, new in [('static int ath11k_pci_init(void)', 'int alioth_ath11k_pci_init(void)'),
                     ('static void ath11k_pci_exit(void)', 'void alioth_ath11k_pci_exit(void)')]:
        if pc.count(old) != 1:
            raise SystemExit('Linux 7.0.14 PCI entry differs: ' + old)
        pc = pc.replace(old, new)
    # Call sites (module_exit, the error label) still name the old functions; one word-boundary
    # rename covers definitions and uses without double-prefixing.
    pc = re.sub(r'\bath11k_pci_exit\b', 'alioth_ath11k_pci_exit', pc)
    pc = re.sub(r'\bath11k_pci_init\b', 'alioth_ath11k_pci_init', pc)
    # ath11k_pci_select_window writes 0x310c only when the window number changes. It starts at 0,
    # and the BHI offset on this part also maps to window 0, so the first high access would assume
    # a window the reset never programmed. A value no window number can equal forces the first
    # select to write the register, which is the state the first access needs.
    reset_window = '\tab_pci->register_window = 0;\n'
    if pc.count(reset_window) != 1:
        raise SystemExit('ath11k window reset ABI differs')
    pc = pc.replace(reset_window, '\tab_pci->register_window = 0xffffffffu;\n')
    (gen / 'pci.c').unlink()
    (gen / 'pci.c').write_text(pc)
    wmi = (source / 'wmi.c').read_text()
    if wmi.count('wait_event_timeout(') != 2:
        raise SystemExit('ath11k WMI command wait ABI differs')
    (gen / 'wmi.c').unlink()
    wmi = wmi.replace('wait_event_timeout(', 'wait_event_timeout_once(')
    trace = '\ttrace_ath11k_wmi_cmd(ab, cmd_id, skb->data, skb->len);'
    if wmi.count(trace) != 1:
        raise SystemExit('ath11k WMI submission trace ABI differs')
    wmi = 'extern void alioth_ath11k_wmi_submitted(unsigned int);\n' + wmi.replace(
        trace, trace + '\n\talioth_ath11k_wmi_submitted(cmd_id);')
    (gen / 'wmi.c').write_text(wmi)
    mhi_driver = (source / 'mhi.c').read_text()
    status = '\tstruct ath11k_pci *ab_pci = ath11k_pci_priv(ab);\n\n\tath11k_dbg(ab, ATH11K_DBG_BOOT, "notify status reason %s\\n",'
    if mhi_driver.count(status) != 1:
        raise SystemExit('ath11k MHI status callback ABI differs')
    mhi_driver = 'struct mhi_controller;\nextern void alioth_ath11k_mhi_failed(struct mhi_controller *, int);\n' + mhi_driver.replace(
        status, '\tstruct ath11k_pci *ab_pci = ath11k_pci_priv(ab);\n\n'
        '\tif (cb == MHI_CB_SYS_ERROR || cb == MHI_CB_FATAL_ERROR || cb == MHI_CB_EE_RDDM) {\n'
        '\t\talioth_ath11k_mhi_failed(mhi_cntrl, cb);\n\t\treturn;\n\t}\n\n'
        '\tath11k_dbg(ab, ATH11K_DBG_BOOT, "notify status reason %s\\n",')
    # MHI must read and write the BAR through its window. This board's QCA6390 exposes 1 MiB of
    # BAR0; offsets from ATH11K_PCI_WINDOW_START (0x80000) up are one 512 KiB window selected
    # through 0x310c, not the address the offset names. The BHI block sits at the offset MHICFG
    # reports (0x28), and readl on a window that was never selected is an access to no register:
    # in in30 the CPU stopped at MHI's first register operation. ath11k_pcic_read32/write32 pick
    # the window and are the same pair every other ath11k register access uses.
    op_read = '''static int ath11k_mhi_op_read_reg(struct mhi_controller *mhi_cntrl,
				  void __iomem *addr,
				  u32 *out)
{
	*out = readl(addr);

	return 0;
}

static void ath11k_mhi_op_write_reg(struct mhi_controller *mhi_cntrl,
				    void __iomem *addr,
				    u32 val)
{
	writel(val, addr);
}'''
    op_new = '''static int ath11k_mhi_op_read_reg(struct mhi_controller *mhi_cntrl,
				  void __iomem *addr,
				  u32 *out)
{
	struct ath11k_base *ab = dev_get_drvdata(mhi_cntrl->cntrl_dev);

	*out = ath11k_pcic_read32(ab, (unsigned long)addr - (unsigned long)ab->mem);

	return 0;
}

static void ath11k_mhi_op_write_reg(struct mhi_controller *mhi_cntrl,
				    void __iomem *addr,
				    u32 val)
{
	struct ath11k_base *ab = dev_get_drvdata(mhi_cntrl->cntrl_dev);

	ath11k_pcic_write32(ab, (unsigned long)addr - (unsigned long)ab->mem, val);
}'''
    if mhi_driver.count(op_read) != 1:
        raise SystemExit('ath11k MHI register op ABI differs')
    mhi_driver = mhi_driver.replace(op_read, op_new)
    (gen / 'mhi.c').unlink()
    (gen / 'mhi.c').write_text(mhi_driver)
    ath_mac = (source / 'mac.c').read_text()
    ops = 'static const struct ieee80211_ops ath11k_ops = {'
    bridge = '''static void alioth_ath11k_vif_cfg_changed(struct ieee80211_hw *hw,
        struct ieee80211_vif *vif, u64 changed)
{
    ath11k_mac_op_bss_info_changed(hw, vif, &vif->bss_conf, changed);
}

'''
    if ath_mac.count(ops) != 1:
        raise SystemExit('ath11k mac80211 operations ABI differs')
    ath_mac = ath_mac.replace(ops, bridge + ops)
    ath_mac, changes = re.subn(
        r'(\t\.bss_info_changed\s*= ath11k_mac_op_bss_info_changed,)',
        r'\1\n\t.link_info_changed = ath11k_mac_op_bss_info_changed,\n'
        r'\t.vif_cfg_changed = alioth_ath11k_vif_cfg_changed,', ath_mac)
    if changes != 1:
        raise SystemExit('ath11k BSS callback ABI differs')
    (gen / 'mac.c').unlink()
    (gen / 'mac.c').write_text(ath_mac)


def patch_shared(shared, out, linux, pinned):
    mac = (shared / 'src/nw_mac80211.c').read_text()
    mac += r"""
/* Linux ath11k still uses these mac80211 APIs; the shared station has one
 * hardware and an explicit active-vif list. Honour the supplied vif address. */
struct ieee80211_sta *ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const u8 *addr, const u8 *local) {
    if (hw != g_hw || !g_sta || !ether_addr_equal(g_sta->addr, addr)) return 0;
    for (int i = 0; i < g_nvif; ++i) if (!local || ether_addr_equal(g_vif[i]->addr, local)) return g_sta;
    return 0;
}
void ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work) { if (hw == g_hw) lx_queue_work(work); }
"""
    (out / 'nw_mac80211_alioth.c').write_text(mac)
    sta = (shared / 'src/nw_sta.c').read_text()
    begin = sta.index('static void make_nonce(u8 *out) {')
    end = sta.index('static void derive_ptk(void)', begin)
    sta = sta[:begin] + 'extern void AliothWifiRandom(void *, size_t);\nstatic void make_nonce(u8 *out) { AliothWifiRandom(out, 32); }\n' + sta[end:]
    join = 'static void start_connect(void) {\n  if (C.state != NW_STA_IDLE && C.state != NW_STA_SCANNING) return;'
    leave = 'else if (kind == REQ_DISCONNECT && C.state >= NW_STA_AUTH) { teardown(0, 0); C.state = NW_STA_IDLE; }'
    if sta.count(join) != 1 or sta.count(leave) != 1:
        raise SystemExit('shared station transition ABI differs')
    sta = sta.replace(join, '''static void start_connect(void) {
  if (C.state == NW_STA_UP && C.ssid_len == g_req.ssid_len &&
      !memcmp(C.ssid, g_req.ssid, C.ssid_len) &&
      (!g_req.has_bssid || !memcmp(C.ap.bssid, g_req.bssid, 6))) return;
  if (C.state >= NW_STA_AUTH) {
    lx_cancel_delayed(&g_timer);
    teardown(C.state == NW_STA_UP, -1);
    C.state = NW_STA_IDLE;
  }
  if (C.state != NW_STA_IDLE && C.state != NW_STA_SCANNING) return;''')
    sta = sta.replace(leave, '''else if (kind == REQ_DISCONNECT && C.state >= NW_STA_AUTH) {
      lx_cancel_delayed(&g_timer); teardown(C.state == NW_STA_UP, -1); C.state = NW_STA_IDLE;
    }''')
    rates_begin = '    const u8 *e = find_ie(C.ap.ie, C.ap.ie_len, WLAN_EID_SUPP_RATES);'
    rates_end = '    g_vif->bss_conf.assoc_capability = C.ap.caps;'
    assign = '  r = g_ops->assign_vif_chanctx(g_hw, g_vif, &g_vif->bss_conf, C.ctx);'
    if sta.count(rates_begin) != 1 or sta.count(rates_end) != 1 or sta.count(assign) != 1:
        raise SystemExit('shared station basic-rate/channel-start ABI differs')
    rate_selection = sta[sta.index(rates_begin):sta.index(rates_end)]
    sta = sta.replace(assign, rate_selection +
        '  lx_log(6, "alioth station: pre-start band=%u basic_rates=0x%x", band, g_vif->bss_conf.basic_rates);\n'
        '  g_ops->link_info_changed(g_hw, g_vif, &g_vif->bss_conf, BSS_CHANGED_BASIC_RATES);\n' + assign)
    (out / 'nw_sta_alioth.c').write_text(sta)
    (out / 'lx_rt_alioth.c').write_text(adapt_dispatcher_timers((shared / 'shim/src/lx_rt.c').read_text()))
    if (shared / 'out/pinned/linux/netdevice_enums.h').is_file():
        return
    pinned.mkdir(exist_ok=True)
    inputs = {'linux/mhi.h': 'linux/mhi.h', 'linux/ieee80211.h': 'linux/ieee80211.h',
              'linux/soc/qcom/qmi.h': 'linux/soc/qcom/qmi.h', 'net/cfg80211.h': 'net/cfg80211.h',
              'net/regulatory.h': 'net/regulatory.h', 'net/codel.h': 'net/codel.h',
              'net/ieee80211_radiotap.h': 'net/ieee80211_radiotap.h',
              'linux/nl80211.h': 'uapi/linux/nl80211.h', 'linux/rfkill.h': 'uapi/linux/rfkill.h',
              'uapi/linux/rfkill.h': 'uapi/linux/rfkill.h', 'net/mac80211.real.h': 'net/mac80211.h',
              'linux/kconfig.h': 'linux/kconfig.h', 'linux/sizes.h': 'linux/sizes.h'}
    inputs.update({str(f.relative_to(linux / 'include')): str(f.relative_to(linux / 'include'))
                   for f in (linux / 'include/linux').glob('ieee80211-*.h')})
    for target, name in inputs.items():
        src = linux / 'include' / name
        if not src.is_file():
            raise SystemExit('Missing Linux public header: ' + str(src))
        dest = pinned / target
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.unlink(missing_ok=True)
        dest.symlink_to(src)


def patch_mhi_boot(source):
    """Put one log line before each step of the SBL load.

    The console stops after the AMSS file is read and before READY, and a wedged MMIO access is
    indistinguishable from a wedged DMA without knowing which step ran last. Every line names the
    step, so the last line of the round is the answer.
    """
    def once(old, new):
        nonlocal source
        if source.count(old) != 1:
            raise SystemExit('MHI boot.c ABI differs: ' + old[:60].replace('\n', '\\n'))
        source = source.replace(old, new)

    once('\tint ret;\n\n\tif (MHI_PM_IN_ERROR_STATE(mhi_cntrl->pm_state)) {\n',
         '\tint ret;\n\n'
         '\tlx_log(6, "mhi fw: ee=%u pm=%u sbl=%zu seg=%zu fw_sz=%zu fw_data=%d",\n'
         '\t       (unsigned)mhi_cntrl->ee, (unsigned)mhi_cntrl->pm_state,\n'
         '\t       mhi_cntrl->sbl_size, mhi_cntrl->seg_len, mhi_cntrl->fw_sz,\n'
         '\t       mhi_cntrl->fw_data != NULL);\n\n'
         '\tif (MHI_PM_IN_ERROR_STATE(mhi_cntrl->pm_state)) {\n')
    once('\t/* save hardware info from BHI */\n'
         '\tret = mhi_read_reg(mhi_cntrl, mhi_cntrl->bhi, BHI_SERIALNU,\n'
         '\t\t\t   &mhi_cntrl->serial_number);\n'
         '\tif (ret)\n'
         '\t\tdev_err(dev, "Could not capture serial number via BHI\\n");\n',
         '\t/* save hardware info from BHI */\n'
         '\tret = mhi_read_reg(mhi_cntrl, mhi_cntrl->bhi, BHI_SERIALNU,\n'
         '\t\t\t   &mhi_cntrl->serial_number);\n'
         '\tif (ret)\n'
         '\t\tdev_err(dev, "Could not capture serial number via BHI\\n");\n'
         '\tlx_log(6, "mhi fw: serial %#x bhi off %#lx bhie off %#lx",\n'
         '\t       mhi_cntrl->serial_number,\n'
         '\t       (unsigned long)mhi_cntrl->bhi - (unsigned long)mhi_cntrl->regs,\n'
         '\t       (unsigned long)mhi_cntrl->bhie - (unsigned long)mhi_cntrl->regs);\n')
    once('\tret = mhi_alloc_bhi_buffer(mhi_cntrl, &image, size);\n\tif (ret)\n\t\treturn ret;\n',
         '\tret = mhi_alloc_bhi_buffer(mhi_cntrl, &image, size);\n\tif (ret)\n\t\treturn ret;\n'
         '\tlx_log(6, "mhi bhi: buffer %#llx len %zu",\n'
         '\t       (unsigned long long)image->mhi_buf[0].dma_addr, image->mhi_buf[0].len);\n')
    once('\tmhi_write_reg(mhi_cntrl, base, BHI_STATUS, 0);\n',
         '\tlx_log(6, "mhi bhi: load addr %#llx size %zu session %u",\n'
         '\t       (unsigned long long)mhi_buf->dma_addr, mhi_buf->len, session_id);\n'
         '\tmhi_write_reg(mhi_cntrl, base, BHI_STATUS, 0);\n')
    once('\tmhi_write_reg(mhi_cntrl, base, BHI_IMGTXDB, session_id);\n\tread_unlock_bh(pm_lock);\n',
         '\tmhi_write_reg(mhi_cntrl, base, BHI_IMGTXDB, session_id);\n\tread_unlock_bh(pm_lock);\n'
         '\tlx_log(6, "mhi bhi: doorbell rung, waiting for BHI_STATUS");\n')
    once('\tmhi_write_reg(mhi_cntrl, base, BHIE_TXVECADDR_HIGH_OFFS,\n',
         '\tlx_log(6, "mhi bhie: load addr %#llx size %zu session %u",\n'
         '\t       (unsigned long long)mhi_buf->dma_addr, mhi_buf->len, sequence_id);\n'
         '\tmhi_write_reg(mhi_cntrl, base, BHIE_TXVECADDR_HIGH_OFFS,\n')
    once('\t\t\t\t  BHIE_TXVECDB_SEQNUM_BMSK, sequence_id);\n\tread_unlock_bh(pm_lock);\n',
         '\t\t\t\t  BHIE_TXVECDB_SEQNUM_BMSK, sequence_id);\n\tread_unlock_bh(pm_lock);\n'
         '\tlx_log(6, "mhi bhie: doorbell rung, waiting for TXVECSTATUS");\n')
    once('\tif (fw_load_type == MHI_FW_LOAD_BHIE)\n'
         '\t\tret = mhi_load_image_bhie(mhi_cntrl, fw_data, size);\n'
         '\telse\n'
         '\t\tret = mhi_load_image_bhi(mhi_cntrl, fw_data, size);\n',
         '\tif (fw_load_type == MHI_FW_LOAD_BHIE)\n'
         '\t\tret = mhi_load_image_bhie(mhi_cntrl, fw_data, size);\n'
         '\telse\n'
         '\t\tret = mhi_load_image_bhi(mhi_cntrl, fw_data, size);\n'
         '\tlx_log(6, "mhi fw: image load ret %d type %d", ret, (int)fw_load_type);\n')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--linux', type=Path, default=Path(os.environ.get('ALIOTH_LINUX', '')))
    parser.add_argument('--shared', type=Path, default=Path(os.environ.get('ALIOTH_SHARED', '')))
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--clang', default=os.environ.get('ALIOTH_CLANG', 'clang'))
    parser.add_argument('--target', default='arm64-apple-ios17.0')
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--compile-only', action='store_true', default=True)
    parser.add_argument('--archive', action='store_true')
    args = parser.parse_args()
    linux, shared = args.linux.resolve(), args.shared.resolve()
    if not (linux / 'drivers/net/wireless/ath/ath11k/pci.c').is_file():
        raise SystemExit('Linux ath11k PCI sources required under --linux (fetch-sources.sh)')
    for name in ('include/nw_abi.h', 'shim/src/lx_rt.c', 'src/nw_sta.c'):
        if not (shared / name).is_file():
            raise SystemExit('Ath12kWifi shared ABI required under --shared: ' + name)
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # The archive step globs this directory, so an object from an older recipe would ride along
    # and collide with the kernel (link-time duplicate symbols). Start from an empty set.
    for stale in out.glob('*.o'):
        stale.unlink()
    source = linux / 'drivers/net/wireless/ath/ath11k'
    gen = out / 'ath11k'
    gen.mkdir(exist_ok=True)
    patch_ath11k(source, gen)
    # The component generates out/pinned with tools/pin-headers.sh (it synthesises
    # linux/netdevice_enums.h from netdevice.h and links the public headers the shim keeps as they
    # are). Its own output is authoritative; fall back to the hand list only when it is absent.
    pinned = shared / 'out/pinned'
    if not (pinned / 'linux/netdevice_enums.h').is_file():
        pinned = out / 'pinned'
    patch_shared(shared, out, linux, pinned)
    mhi = out / 'mhi'
    mhi.mkdir(exist_ok=True)
    for target, link in [(n + '.c', n + '.c') for n in MHI] + [('internal.h', 'internal.h')]:
        dest = mhi / link
        dest.unlink(missing_ok=True)
        dest.symlink_to(linux / 'drivers/bus/mhi/host' / target)
    # in30 and in31 both stopped the CPU after the AMSS file was read and before
    # "Wait for device to enter SBL or Mission mode": that is inside the SBL load, and the console
    # cannot say which register or DMA operation wedged. One line before each step turns the last
    # line of the next round into the answer.
    (mhi / 'boot.c').unlink(missing_ok=True)
    (mhi / 'boot.c').write_text(patch_mhi_boot((linux / 'drivers/bus/mhi/host/boot.c').read_text()))
    (mhi / 'trace.h').write_text((shared / 'shim/mhi/trace.h').read_text())
    (out / 'common.h').unlink(missing_ok=True)
    (out / 'common.h').symlink_to(linux / 'drivers/bus/mhi/common.h')
    spectral = out / 'spectral_common.h'
    spectral.unlink(missing_ok=True)
    spectral.symlink_to(source.parent / 'spectral_common.h')
    overlay = WIFI / 'ath11k/compat'
    # clang's own builtin headers (stdint.h, stddef.h) survive -nostdinc only through its resource
    # directory; the shim/libc supplies the C library side.
    resource = subprocess.check_output([args.clang, '-print-resource-dir'], text=True).strip() + '/include'
    flags = ['-target', args.target, '-mkernel', '-O2', '-g0', '-ffreestanding',
             '-fno-builtin', '-fno-common', '-fno-strict-aliasing', '-fwrapv', '-fno-stack-protector',
             '-fno-delete-null-pointer-checks', '-mgeneral-regs-only', '-std=gnu11',
             '-Wno-incompatible-pointer-types', '-Wno-error', '-Wno-unused-value',
             '-nostdinc', '-isystem', str(shared / 'shim/libc'), '-isystem', resource,
             '-I', str(overlay), '-include', 'PongCompat.h', '-I', str(shared / 'shim/include'),
             '-I', str(shared / 'include'), '-I', str(pinned), '-I', str(shared / 'src'),
             '-I', str(gen), '-I', str(out), '-I', str(source.parent),
             # Any Linux header the shim or the pinned set does not provide resolves from the tree,
             # but only after every shim path has been searched.
             '-idirafter', str(linux / 'include'), '-idirafter', str(linux / 'arch/arm64/include'),
             '-DCONFIG_ATH11K_PCI=1',
             # XNU's kpi_socket exports sock_release(socket_t); the shared QRTR has its own
             # sock_release(struct socket *) for the QIPCRTR sockets. One link holds both, so the
             # shared copy carries a private name here (definition and every caller in these
             # objects go through the same macro).
             '-Dsock_release=lx_qrtr_sock_release',
             '-DLX_BOOT_SETTLE_MS=0']
    jobs = [(gen / (n + '.c'), out / ('ath11k_' + n + '.o'), []) for n in ATH]
    jobs += [(mhi / (n + '.c'), out / ('mhi_' + n + '.o'), ['-I', str(mhi)]) for n in MHI]
    # nw_events/nw_netdev and Linux's rbtree.c are part of the shared module (tools/sources.txt);
    # the station path references them, so they are compiled here too.
    common = ['src/nw_crypto.c', 'src/nw_mac80211.c', 'src/nw_net.c', 'src/nw_netdev.c',
              'src/nw_events.c', 'src/nw_pci.c', 'src/nw_qrtr.c', 'src/nw_qrtr_mhi.c',
              'src/nw_sta.c', 'shim/src/lx_rt.c', 'shim/src/mhi_hook.c']
    replacements = {'src/nw_sta.c': out / 'nw_sta_alioth.c',
                    'src/nw_mac80211.c': out / 'nw_mac80211_alioth.c',
                    'shim/src/lx_rt.c': out / 'lx_rt_alioth.c'}
    jobs += [(replacements.get(n, shared / n), out / ('shared_' + Path(n).stem + '.o'), []) for n in common]
    jobs += [(linux / ('drivers/soc/qcom/' + n + '.c'), out / (n + '.o'), []) for n in ('qmi_interface', 'qmi_encdec')]
    jobs += [(linux / 'lib/rbtree.c', out / 'rbtree.o', [])]
    jobs += [(WIFI / 'ath11k/AliothAth11k.c', out / 'AliothAth11k.o', [])]
    commands = []
    def compile_job(job):
        src, obj, extra = job
        cmd = [args.clang, *flags, *extra, '-c', str(src), '-o', str(obj)]
        commands.append([str(x) for x in cmd])
        result = subprocess.run(cmd, capture_output=True, text=True)
        (out / (obj.stem + '.log')).write_text(result.stdout + result.stderr)
        return obj.name if result.returncode else None
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(args.jobs) as pool:
        failed = [r for r in pool.map(compile_job, jobs) if r]
    (out / 'compile-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    if failed:
        raise SystemExit('ath11k compile failures (logs kept): ' + ', '.join(failed))
    print('PASS: %d ath11k/shared objects compiled' % len(jobs))


if __name__ == '__main__':
    main()
