/*
 * The Wi-Fi's PCIe controller on the Redmi K40 (alioth), poked one step at a time.
 *
 * The K40's radio is a QCA6390 on PCIe0 (vendor 0x17cb, device 0x1101), behind the root complex
 * qcom,pcie@1c00000 -- not the AHB radio (WCN6750) the rest of this tree's Wi-Fi driver was
 * written for (redwood_wifi.c). Everything below is this phone's: the device tree's node
 * qcom,pcie@1c00000 (reg-names parf, phy, dm_core, elbi, iatu, conf), the clock controller's
 * PCIe0 entries (gcc-kona.c) and the PCIe block's offsets (the vendor 4.19 tree's pci-msm.c,
 * the driver this phone runs: msm_pcie_enable, msm_pcie_link_train).
 *
 * Addresses, from the phone's own device tree (read from it with `dtc -I dtb -O dts`):
 *   parf   0x01c00000 + 0x3000       phy   0x01c06000 + 0x1000
 *   dm_core 0x60000000 + 0xf1d (ELBI at 0x60000f20 + 0xa8)   iatu 0x60001000 + 0x1000
 *   conf   0x60100000 + 0x100000 (the configuration space, bus 0 onwards; this
 *          file reads only bus 0 device 0 function 0, so it maps 4 KiB of it)
 *   memory 0x60300000 + 0x3d00000, i/o 0x60200000 + 0x100000 (both the same on the bus side)
 *   perst-gpio is TLMM 0x4f (79), wake-gpio 0x51 (81); root complex 0
 *
 * From the clock controller (gcc-kona.c; all of these are offsets from its 0x100000):
 *   pcie_0_gdscr 0x6b004 (the controller's own power switch), the five clock votes in
 *   gcc_pcie_0_axi_clk_misc 0x52008 (bit 0 slv_axi, 1 mstr_axi, 2 cfg_ahb, 3 aux, 4 pipe, 5
 *   slv_q2a_axi), the phy's own two clocks at 0x6f004 and 0x6f02c, the bus clock at 0x9000c,
 *   and the two block resets 0x6b000 (core) and 0x6c01c (phy).
 *
 * The steps are how this is to be found out, and one that fails says so and stops the sequence:
 *   1  reads   what the boot loader and Android left of all of that: the power switch, the clock
 *              votes and their halt state, the two resets, the PHY's status word -- and, only if
 *              the controller is actually on, a few of its own registers (reading them while its
 *              clocks are off is a bus access that never returns)
 *   2  writes  the controller's power and clocks on and its two resets out, then reads it back
 *   3  writes  the PHY's own sequence (the device tree's qcom,phy-sequence) and asks it for the
 *              "ready" bit
 *   4  writes  the PHY's auxiliary clock and its power-down word, then waits for "ready"
 *   5  writes  the PHY's own sequence, then waits for "ready" again
 *   6  reads   the whole PCIe0 corner of the clock controller
 *   7  switches the auxiliary clock's own source generator on
 *   8  reads   the controller's own registers (PARF)
 *   9  writes  the vendor driver's init sequence into PARF
 *  10  drives  the endpoint's two pads: WLAN_EN (TLMM 20) high, then PERST# (TLMM 79) released,
 *              and reads the link state. This is the step that first powers the endpoint.
 *  11  writes  the vendor's DBI training words and PARF_LTSSM bit 8, then polls ELBI/DLLLA/ID
 *  12  reads   what the command database says about the two PMIC rails (ldoa5, ldoa9)
 *  13  repeats the seven dm_core writes as a separate, opt-in diagnostic
 *  14  reads   the link control block's status word (ELBI), the signal the vendor polls
 *  15  reads   the configuration space: the root complex's and the QCA6390's vendor and device
 *
 * Boot-arg pcie=N performs steps 1 to N and stops; pcie_pause=MS is how long a step waits after
 * announcing a group of accesses (default 2200, as the loader notes the log every two seconds).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET) || defined(ARM64_BOARD_CONFIG_BCM2837)
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

/*
 * The command database lookup, reused from GpuPowerBoard.h rather than reimplemented: it is a
 * pure-C static inline (no class, no namespace) and GpuProbe already proved it on this board --
 * it resolves gfx.lvl, mx.lvl, cx.lvl, gmxc.lvl, SH0, MC0, ACV and CN0 every boot.
 */
#include "GpuPowerBoard.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void thread_terminate(thread_t thread);
/*
 * bsd/dev/sweet_ufs.c: the persistent log. printf already appends to its ring
 * (osfmk/console/serial_console.c _cnputs), so a line only needs the flush that
 * writes the ring out to the partition. This replaces the loader log the
 * reference used (loader_return.c), which this tree does not have.
 */
extern int sweet_log_flush(void);
/* The persistent log's writer owns the UFS window. This waits for its next round, so a line
 * already in the kernel message buffer is on the medium before the caller continues. */
extern int boot_log_flush_wait(void) __attribute__((weak_import));
/* This board's console. printf() does not reach it (a DEVELOPMENT kernel leaves
 * disable_kprintf_output set); this is the channel that does. */
extern size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));

static uint32_t pcie_last_step, pcie_step, pcie_pause_ms = 2200;
static uint32_t pcie_skip_steps;

static void run_steps(void);
static void pcie_steps_thread(void *argument, wait_result_t wait_result);
static bool early_power_vote(void);

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static void
announce(const char *what);

static void
pause_group(void)
{
	if (pcie_step == pcie_last_step && pcie_pause_ms) {
		sleep_ms(pcie_pause_ms);
	}
}

#define BLOCK_NAMES 8
static struct {
	const char *name;
	uint64_t base;
	uint32_t size;
	vm_offset_t va;
} blocks[BLOCK_NAMES] = {
	{ "gcc", 0x00100000, 0x00100000 },   /* highest offset read: 0x9000c */
	{ "parf", 0x01c00000, 0x3000 },
	{ "phy", 0x01c06000, 0x1000 },
	{ "elbi", 0x60000f20, 0xa8 },
	{ "iatu", 0x60001000, 0x1000 },
	{ "conf", 0x60100000, 0x1000 },      /* bus 0 device 0 function 0 only */
	{ "dbi", 0x60000000, 0x1000 },   /* the core's own 4 KB: DBI at its start, ELBI at 0xf20 */
	/*
	 * The TLMM tile the pads this file needs live in. The pads are split into tiles
	 * (pinctrl-kona.c:20-22: NORTH 0x00900000, SOUTH 0x00500000) at a 0x1000 stride, from the
	 * TLMM base 0x0f000000 (kona-pinctrl.dtsi:2), so NORTH starts at 0x0f900000. gpio 20, 79
	 * and 124 are all NORTH (pinctrl-kona.c:1461, :1576, :1580). The size covers pin 124 at
	 * +0x7c000; the touch driver maps the same tile (FocalTouchBoard.h:56-59).
	 */
	{ "tlmm", 0x0f900000, 0x80000 },
};

static uint64_t
reg(int which, uint32_t offset)
{
	if (blocks[which].va == 0 || offset >= blocks[which].size) {
		return 0;
	}
	return blocks[which].va + offset;
}

static uint32_t
rd(uint64_t address)
{
	return address ? *(volatile uint32_t *)address : 0;
}

static void
wr(uint64_t address, uint32_t value)
{
	if (address) {
		*(volatile uint32_t *)address = value;
	}
}

static void
say(const char *fmt, ...)
{
	__builtin_va_list ap;

	printf("pcie: %u: ", pcie_step);
	__builtin_va_start(ap, fmt);
	vprintf(fmt, ap);
	__builtin_va_end(ap);
}

/*
 * A line goes into the console, which keeps it in the log ring in memory. It
 * reaches the partition only when something flushes the ring, so anything
 * printed just before a register access is flushed first (see risky()): a
 * processor that stops on an access that never returns takes whatever it
 * printed last with it otherwise.
 */
static void
line(const char *fmt, ...)
{
	__builtin_va_list ap;

	printf("pcie: %u: ", pcie_step);
	__builtin_va_start(ap, fmt);
	vprintf(fmt, ap);
	__builtin_va_end(ap);
	printf("\n");
	if (&qcom_usb_debug_write) {
		char body[192], out[224];
		int n;
		__builtin_va_start(ap, fmt);
		n = vsnprintf(body, sizeof body, fmt, ap);
		__builtin_va_end(ap);
		if (n > 0) {
			int m = snprintf(out, sizeof out, "PCIE %u: %s\n", pcie_step, body);
			if (m > 0) qcom_usb_debug_write(out, (size_t)m < sizeof(out) ? (size_t)m : sizeof(out) - 1);
		}
	}
}

/* Print, wait for the persistent-log writer's next round, then use the USB console. The wait is
 * before the USB write on purpose: if the USB path is the thing that does not return, the line is
 * already on the medium. */
static void
line_persist(const char *fmt, ...)
{
	char body[192], out[224];
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	vsnprintf(body, sizeof body, fmt, ap);
	__builtin_va_end(ap);
	printf("pcie: %u: %s\n", pcie_step, body);
	if (&boot_log_flush_wait) {
		(void)boot_log_flush_wait();
	}
	if (&qcom_usb_debug_write) {
		const int m = snprintf(out, sizeof out, "PCIE %u: %s\n", pcie_step, body);
		if (m > 0) {
			qcom_usb_debug_write(out, (size_t)m < sizeof(out) ? (size_t)m : sizeof(out) - 1);
		}
	}
}

/* Put the ring on the medium now: the access that follows may never return. */
static void
risky(void)
{
	(void)sweet_log_flush();
}

static uint32_t persisted_access_step;

/* The last step is the one whose first access is allowed to be fatal. Print its "about to"
 * line with the persistent wait, once; later accesses in the same step keep the normal path. */
static bool
persist_access(void)
{
	if (pcie_step != pcie_last_step || persisted_access_step == pcie_step) {
		return false;
	}
	persisted_access_step = pcie_step;
	return true;
}

static void
announce(const char *what)
{
	if (pcie_step != pcie_last_step) {
		return;
	}
	line_persist("%s", what);
}

static uint32_t
get(int which, const char *name, uint32_t offset)
{
	uint64_t address = reg(which, offset);
	uint32_t value;

	if (persist_access()) {
		line_persist("about to read %s %s at %#llx", blocks[which].name, name, address);
	} else {
		line("about to read %s %s at %#llx", blocks[which].name, name, address);
	}
	risky();
	value = rd(address);
	line("%s %s (%#llx) = %#x", blocks[which].name, name, address, value);
	return value;
}

static void
put(int which, const char *name, uint32_t offset, uint32_t value)
{
	uint64_t address = reg(which, offset);

	if (persist_access()) {
		line_persist("about to write %s %s at %#llx: %#x", blocks[which].name, name, address, value);
	} else {
		line("about to write %s %s at %#llx: %#x", blocks[which].name, name, address, value);
	}
	risky();
	wr(address, value);
}

/* Where the clock controller's registers are, as an address (its block base plus the offset). */
#define GCC(off)                reg(0, (off))

static bool
map_all(void)
{
	if (blocks[0].va != 0) {
		return true;    /* already mapped: io_submap is 8 MiB and this window is not free */
	}
	for (int i = 0; i < BLOCK_NAMES; i++) {
		blocks[i].va = ml_io_map(blocks[i].base, blocks[i].size);
		if (blocks[i].va == 0) {
			printf("pcie: NO MEMORY FOR THE %s BLOCK AT %#llx\n", blocks[i].name, blocks[i].base);
			return false;
		}
	}
	return true;
}

/* ---- step 1: what is left of the controller ------------------------------------------------ */

#define GCC_PCIE_0_GDSCR        0x6b004
#define GCC_PCIE_0_AXI_MISC     0x52008
#define GCC_PCIE_0_Q2A_AXI_HALT 0x6b010
#define GCC_PCIE_0_SLV_AXI_HALT 0x6b014
#define GCC_PCIE_0_MSTR_HALT    0x6b01c
#define GCC_PCIE_0_CFG_AHB_HALT 0x6b024
#define GCC_PCIE_0_AUX_HALT     0x6b028
#define GCC_PCIE_0_PIPE_HALT    0x6b02c
#define GCC_PCIE_PHY_AUX        0x6f004
#define GCC_PCIE_0_PHY_REFGEN   0x6f02c
#define GCC_PCIE_TBU            0x9000c
/* The device tree's clock-names entry "pcie_0_ldo" is GCC_PCIE_WIFI_CLKREF_EN (gcc-kona.h:86,
 * clock id 74), and gcc-kona.c:2521 puts its branch at 0x8c004 with mask BIT(0). It is the one
 * entry of the vendor's eleven that this file never touches. */
#define GCC_PCIE_WIFI_CLKREF    0x8c004
#define GCC_PCIE_0_BCR          0x6b000
#define GCC_PCIE_0_PHY_BCR      0x6c01c
#define GCC_PCIE_PHY_BCR        0x6f000
#define GCC_PCIE_PHY_COM_BCR    0x6f010

#define GDSC_POWER_ON           (1u << 31)
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_RETAIN_FF_ENABLE   (1u << 11)

/* TLMM pad layout: [1:0] pull, [5:2] function, [8:6] drive, [9] output enable
 * (pinctrl-kona.c:32-38). REG_SIZE 0x1000, io register at +0x4, out bit 1, in bit 0
 * (pinctrl-kona.c:29-38, pinctrl-msm.c:373-378, :299-300). */
#define TLMM_CFG(pin)           ((pin) * 0x1000u)
#define TLMM_IO(pin)            ((pin) * 0x1000u + 4u)
#define TLMM_PULL_DOWN          1u
#define TLMM_PULL_UP            3u
#define TLMM_FUNC_GPIO          0u
#define TLMM_DRIVE(d)           ((((d) / 2u) - 1u) << 6)
#define TLMM_OE                 (1u << 9)
#define TLMM_OUT                0x2u           /* bit 1 of the io register */
#define TLMM_IN                 0x1u           /* bit 0 of the io register */

/* The three pads this file drives, from the vendor device tree:
 *   gpio 79  perst-gpio, active low   (kona-pcie.dtsi:36, pci-msm.c:939 on=0)
 *   gpio 20  wlan-en-gpio, active high (kona.dtsi:4894, output-high in cnss_wlan_en_active)
 *   gpio124  qcom,sw-ctrl-gpio         (kona.dtsi:4896) */
#define GPIO_PERST              79u
#define GPIO_WLAN_EN            20u

/* The DWC's own configuration window (dm_core), the registers the vendor writes before it
 * reads ELBI for the first time (pci-msm.c:4379-4406). Offsets from pci-msm.c:47-50,93,100. */
#define DWC_GEN3_SPCIE_CAP      0x0154          /* pci-msm.c:47 */
#define DWC_DEVICE_CONTROL_STTS 0x0078          /* pci-msm.c:100 */
#define DWC_GEN3_GEN2_CTRL      0x080c          /* pci-msm.c:48 */
#define DWC_GEN3_RELATED        0x0890          /* pci-msm.c:49 */
#define DWC_GEN3_EQ_CONTROL     0x08a8          /* pci-msm.c:50 */
#define DWC_GEN3_MISC_CONTROL   0x08bc          /* pci-msm.c:52 */
#define DWC_CORE_PRESET         0x55555555      /* PCIE_GEN3_PRESET_DEFAULT, pci-msm.c:42 */
#define DWC_TLP_RD_SIZE         0x5             /* PCIE_TLP_RD_SIZE, pci-msm.c:198 */

/* RPMh, from this board's DT and the vendor regulator driver. The RSC layout is v2 on this
 * board and GpuProbe proved every offset of it here (its rpmhVoteDdr runs each boot). */
#define RSC_DRV2_BASE           0x18220000      /* DT apps_rsc reg-names drv-2 */
#define RSC_TCS_OFFSET          0xd00           /* DT qcom,tcs-offset */
#define RSC_TCS_STRIDE          672             /* v2 layout, QcomRpmh.c:56 */
#define RSC_CMD_STRIDE          20              /* v2 layout */
#define RSC_DRV_IRQ_STATUS      0x04
#define RSC_DRV_IRQ_ENABLE      0x00
#define RSC_DRV_IRQ_CLEAR       0x08
#define RSC_TCS_IRQ_STATUS      RSC_DRV_IRQ_STATUS
#define RSC_DRV_CONFIG          0x0c            /* DRV_PRNT_CHLD_CONFIG */
#define RSC_TCS_CONTROL         0x14            /* v2 */
#define RSC_TCS_STATUS          0x18
#define RSC_TCS_CMD_WAIT        0x10            /* CMD_WAIT_FOR_CMPL, vendor rpmh-rsc.c:47 */
#define RSC_TCS_CMD_ENABLE      0x1c            /* v2 */
#define RSC_TCS_IRQ_CLEAR       0x08
#define RSC_CMD_MSGID           0x30
#define RSC_CMD_ADDR            0x34
#define RSC_CMD_DATA            0x38
#define RSC_CMD_STATUS          0x3c
#define RSC_CONTROL_AMC_ON      (1u << 16)
#define RSC_CONTROL_TRIGGER     (1u << 24)
#define RSC_CMD_MSGID_WRITE_RESPONSE (8u | (1u << 8) | (1u << 16))

/* The two rails the vendor turns on before it touches dm_core (pci-msm.c:930-935 required,
 * kona-pcie.dtsi:47-48). Both are VRM (qcom,rpmh-vrm-regulator, pmic5-ldo), so their registers
 * are voltage/enable/mode at +0/+4/+8 (rpmh-regulator.c:87-99), NOT an ARC level index --
 * rpmh-regulator.c:1926-1928 only loads the aux level mapping for ARC. */
#define RAIL_LDOA5_ADDR         0x41800
#define RAIL_LDOA9_ADDR         0x41b00
#define RAIL_VRM_VOLTAGE        0               /* +0, millivolts (rpmh-regulator.c:986) */
#define RAIL_VRM_ENABLE         4               /* +4 */
#define RAIL_VRM_MODE           8               /* +8 */
#define RAIL_LDOA5_MV           880             /* DT pm8150_l5, min = max = 880000 uV */
#define RAIL_LDOA9_MV           1200            /* DT pm8150_l9, min = max = 1200000 uV */
#define RAIL_CX_NAME            "cx.lvl"        /* kona-pcie.dtsi:49, kona-regulators.dtsi:51 */
#define RAIL_CX_LEVEL           256             /* RPMH_REGULATOR_LEVEL_NOM, kona-pcie.dtsi:52-53 */
#define RAIL_ENABLE_ON          1
#define RAIL_MODE_PMIC5_LDO_HPM 7               /* rpmh-regulator.c:281; NOT the enum HPM = 4 */

#define PHY_STATUS              0x814           /* dt: qcom,phy-status-offset; bit 6: ready */
#define PHY_POWER_DOWN          0x840           /* dt: qcom,phy-power-down-offset */

#define GDSC_PWR_ON             (1u << 31)      /* gdsc.c: PWR_ON_MASK */
#define CLOCK_VOTES             0x2fu           /* slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a (pipe comes with the PHY) */

static bool
step_look(void)
{
	uint32_t misc, gdscr;

	announce("read what the boot loader and Android left of the Wi-Fi's PCIe controller: its power switch, the clock "
	    "votes and their halt state, its two resets, and the PHY's status word");
	gdscr = get(0, "pcie_0_gdscr", GCC_PCIE_0_GDSCR);
	misc = get(0, "pcie_0_axi_misc", GCC_PCIE_0_AXI_MISC);
	get(0, "pcie_0_slv_axi_halt", GCC_PCIE_0_SLV_AXI_HALT);
	get(0, "pcie_0_mstr_axi_halt", GCC_PCIE_0_MSTR_HALT);
	get(0, "pcie_0_cfg_ahb_halt", GCC_PCIE_0_CFG_AHB_HALT);
	get(0, "pcie_0_aux_halt", GCC_PCIE_0_AUX_HALT);
	get(0, "pcie_0_pipe_halt", GCC_PCIE_0_PIPE_HALT);
	get(0, "pcie_0_slv_q2a_halt", GCC_PCIE_0_Q2A_AXI_HALT);
	get(0, "pcie_phy_aux", GCC_PCIE_PHY_AUX);
	get(0, "pcie0_phy_refgen", GCC_PCIE_0_PHY_REFGEN);
	get(0, "aggre_noc_pcie_tbu", GCC_PCIE_TBU);
	get(0, "pcie_wifi_clkref (the device tree's pcie_0_ldo)", GCC_PCIE_WIFI_CLKREF);
	get(0, "pcie_0_bcr (core reset)", GCC_PCIE_0_BCR);
	get(0, "pcie_0_phy_bcr (phy reset)", GCC_PCIE_0_PHY_BCR);
	printf("pcie: %u: the controller is %s (gdscr %#x), its clock votes %#x\n", pcie_step,
	    (gdscr & GDSC_POWER_ON) ? "on" : "off", gdscr, misc);
	{
		/*
		 * The early return below tests (misc & 0x3f) != 0x3f, and this line has only ever been
		 * read as "it returned", never as "what the six vote bits are". If none of them is set,
		 * the controller believes its own clocks are not voted for, and a slave port that thinks
		 * that may simply not answer -- which is a different cause from "the address is wrong"
		 * and is not excluded by the probe below running before step 3 votes anything.
		 */
		uint32_t votes = misc & 0x3fu;

		line("the clock votes are %#x, all six are voted: %s", votes,
		    votes == 0x3fu ? "yes" : "NO -- none of slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a is voted yet");
		/*
		 * The window probe used to run here, with the controller still collapsed AND no clock
		 * voted -- two things at once. It hung, which said the window was unreachable in that
		 * state but could not say which of the two made it unreachable. It now runs in step 2,
		 * after the controller is powered and still before step 3 votes anything: that is one
		 * variable.
		 */
	}
	if ((gdscr & GDSC_POWER_ON) == 0 || (misc & 0x3f) != 0x3f) {
		say("its clocks are not all voted for, so neither its PHY's nor its own registers are read here\n");
		return true;
	}
	pause_group();
	/* parf is block 1 and phy is block 2; the reference had these two swapped here. */
	get(2, "phy_status", PHY_STATUS);
	get(2, "phy_power_down", PHY_POWER_DOWN);
	get(1, "parf_sys_ctrl", 0x00);
	get(1, "parf_phy_ctrl", 0x40);
	get(1, "parf_phy_refclk", 0x4c);
	get(1, "parf_dbi_base", 0x168);
	get(1, "parf_slv_addr_space_size (2.3.2, 0x16c)", 0x16c);
	get(1, "parf_slv_addr_space_size (2.7.0, 0x358)", 0x358);
	get(1, "parf_ltssm", 0x1b0);
	get(1, "parf_q2a_flush", 0x1ac);
	get(1, "parf_bdf_translate", 0x24c);
	get(3, "elbi_sys_ctrl", 0x04);
	return true;
}

static bool
step_gdsc(void)
{
	uint32_t gdscr, waited;

	announce("switch the controller's own power on: the software collapse bit out of its power switch, then wait for its "
	    "power-on bit");
	/*
	 * The vendor's pci-msm.c:930-935 gets vreg-1p8, vreg-0p9 and vreg-cx before it
	 * powers the controller. The external same-board success vote is at its step 2 for
	 * the same reason. This is gated by pcie_power so a normal boot and a probe round
	 * without the switch never touch a PMIC rail.
	 */
	if (!early_power_vote()) {
		return false;
	}
	gdscr = get(0, "pcie_0_gdscr", GCC_PCIE_0_GDSCR);
	put(0, "pcie_0_gdscr", GCC_PCIE_0_GDSCR, gdscr & ~GDSC_SW_COLLAPSE);
	for (waited = 0; waited < 100; waited++) {
		if (rd(GCC(GCC_PCIE_0_GDSCR)) & GDSC_PWR_ON) {
			break;
		}
		sleep_ms(1);
	}
	gdscr = get(0, "pcie_0_gdscr after", GCC_PCIE_0_GDSCR);
	if ((gdscr & GDSC_PWR_ON) == 0) {
		printf("pcie: %u: THE CONTROLLER DID NOT COME ON (gdscr %#x)\n", pcie_step, gdscr);
		return false;
	}
	line("THE CONTROLLER HAS POWER (gdscr %#x, %u ms)", gdscr, waited);
	/*
	 * The window probe that used to run here is gone. wifi6 and wifi7 both hung on it, which
	 * settled what it was for (the window is not reachable in either the collapsed or the
	 * powered-no-votes state). Leaving it in would only hang this round before step 12's
	 * dm_core writes ever ran -- measured on wifi8, which lost its whole round to it.
	 * Step 12 now makes the first write this file has ever made to that window, and that
	 * attempt is the probe: it either lands or it does not.
	 */
	return true;
}

static bool
step_clocks(void)
{
	announce("vote for its clocks (slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a, and the WiFi clock reference the device "
	    "tree calls pcie_0_ldo; the pipe clock comes with the PHY) and read the halt state of each");
	put(0, "pcie_0_axi_misc (vote for its clocks)", GCC_PCIE_0_AXI_MISC, rd(GCC(GCC_PCIE_0_AXI_MISC)) | CLOCK_VOTES);
	/* The WiFi clock reference, which the device tree calls "pcie_0_ldo": a branch of its own at
	 * 0x8c004, not one of the six bits in the vote above. */
	put(0, "pcie_wifi_clkref (vote for the device tree's pcie_0_ldo)", GCC_PCIE_WIFI_CLKREF,
	    rd(GCC(GCC_PCIE_WIFI_CLKREF)) | 1u);
	sleep_ms(1);
	get(0, "pcie_0_axi_misc after", GCC_PCIE_0_AXI_MISC);
	get(0, "pcie_0_slv_axi_halt", GCC_PCIE_0_SLV_AXI_HALT);
	get(0, "pcie_0_mstr_axi_halt", GCC_PCIE_0_MSTR_HALT);
	get(0, "pcie_0_cfg_ahb_halt", GCC_PCIE_0_CFG_AHB_HALT);
	get(0, "pcie_0_aux_halt", GCC_PCIE_0_AUX_HALT);
	get(0, "pcie_0_slv_q2a_halt", GCC_PCIE_0_Q2A_AXI_HALT);
	get(0, "pcie_wifi_clkref after", GCC_PCIE_WIFI_CLKREF);
	return true;
}

static bool
step_phy(void)
{
	uint32_t status = 0;

	announce("wake the PHY: its own auxiliary clock on, its power-down word cleared, and its status bit that says it is "
	    "ready");
	put(0, "pcie_phy_aux (the PHY's clock)", GCC_PCIE_PHY_AUX, rd(GCC(GCC_PCIE_PHY_AUX)) | 1u);
	sleep_ms(1);
	get(0, "pcie_phy_aux after", GCC_PCIE_PHY_AUX);
	pause_group();
	get(2, "phy_power_down", PHY_POWER_DOWN);
	get(2, "phy_status", PHY_STATUS);
	put(2, "phy_power_down (clear)", PHY_POWER_DOWN, rd(reg(2, PHY_POWER_DOWN)) & ~1u);
	for (uint32_t waited = 0; waited < 100; waited++) {
		status = rd(reg(2, PHY_STATUS));
		if (status & (1u << 6)) {
			break;
		}
		sleep_ms(1);
	}
	line("the PHY %s (status %#x, bit 6 is its ready)", (status & (1u << 6)) ? "IS READY" : "IS NOT READY", status);
	return true;
}

static bool
step_phy_seq(void)
{
	/* the phone's own sequence, from the device tree's qcom,phy-sequence (offset, value) */
	static const struct {
		uint16_t reg, value;
	} phy_sequence[] = {
	{ 0x840, 0x03 }, { 0x094, 0x08 }, { 0x154, 0x34 }, { 0x16c, 0x08 },
	{ 0x058, 0x0f }, { 0x0a4, 0x42 }, { 0x110, 0x24 }, { 0x11c, 0x03 },
	{ 0x118, 0xb4 }, { 0x10c, 0x02 }, { 0x1bc, 0x11 }, { 0x0bc, 0x82 },
	{ 0x0d4, 0x03 }, { 0x0d0, 0x55 }, { 0x0cc, 0x55 }, { 0x0b0, 0x1a },
	{ 0x0ac, 0x0a }, { 0x0c4, 0x68 }, { 0x0e0, 0x02 }, { 0x0dc, 0xaa },
	{ 0x0d8, 0xab }, { 0x0b8, 0x34 }, { 0x0b4, 0x14 }, { 0x158, 0x01 },
	{ 0x074, 0x06 }, { 0x07c, 0x16 }, { 0x084, 0x36 }, { 0x078, 0x06 },
	{ 0x080, 0x16 }, { 0x088, 0x36 }, { 0x1b0, 0x1e }, { 0x1ac, 0xca },
	{ 0x1b8, 0x18 }, { 0x1b4, 0xa2 }, { 0x050, 0x07 }, { 0x010, 0x01 },
	{ 0x01c, 0x31 }, { 0x020, 0x01 }, { 0x024, 0xde }, { 0x028, 0x07 },
	{ 0x030, 0x4c }, { 0x034, 0x06 }, { 0x29c, 0x12 }, { 0x284, 0x35 },
	{ 0x23c, 0x11 }, { 0x51c, 0x03 }, { 0x518, 0x1c }, { 0x524, 0x1e },
	{ 0x4e8, 0x00 }, { 0x4ec, 0x0e }, { 0x4f0, 0x4a }, { 0x4f4, 0x0f },
	{ 0x5b4, 0x04 }, { 0x434, 0x7f }, { 0x444, 0x70 }, { 0x510, 0x17 },
	{ 0x4d4, 0x04 }, { 0x4d8, 0x07 }, { 0x598, 0xd4 }, { 0x59c, 0x54 },
	{ 0x5a0, 0xdb }, { 0x5a4, 0x3b }, { 0x5a8, 0x31 }, { 0x584, 0x24 },
	{ 0x588, 0xe4 }, { 0x58c, 0xec }, { 0x590, 0x3b }, { 0x594, 0x36 },
	{ 0x570, 0x3f }, { 0x574, 0x3f }, { 0x578, 0xff }, { 0x57c, 0x7f },
	{ 0x580, 0x14 }, { 0x4fc, 0x00 }, { 0x4f8, 0xc0 }, { 0x460, 0x30 },
	{ 0x464, 0x00 }, { 0x5bc, 0x0c }, { 0x4dc, 0x1b }, { 0x408, 0x0c },
	{ 0x414, 0x03 }, { 0x5b8, 0x30 }, { 0x9a4, 0x01 }, { 0xc90, 0x00 },
	{ 0xc40, 0x01 }, { 0xc48, 0x01 }, { 0xc50, 0x00 }, { 0xcb4, 0x33 },
	{ 0xcbc, 0x00 }, { 0xce0, 0x58 }, { 0xca4, 0x0f }, { 0x048, 0x90 },
	{ 0xc1c, 0xc1 }, { 0x988, 0x77 }, { 0x998, 0x0b }, { 0x8dc, 0x0d },
	{ 0x9ec, 0x12 }, { 0x800, 0x00 }, { 0x844, 0x03 },
	};

	announce("write the PHY's own sequence, the pairs the device tree keeps (qcom,phy-sequence: register and value, all of "
	    "them in the PHY's own 4 KB), then wait for its ready bit");
	/*
	 * The vendor pulses only pcie_0_phy_reset (0x6c01c) with the PHY clocks on, before writing
	 * qcom,phy-sequence. Releasing pcie_phy_bcr (0x6f000) and then pcie_phy_com_bcr (0x6f010)
	 * after the sequence is a different, later generation's order: here it wipes the register
	 * pairs just written. Vote the two PHY clock branches again after the block reset.
	 */
	put(0, "pcie_0_phy_bcr (assert)", GCC_PCIE_0_PHY_BCR, 1);
	sleep_ms(1);
	put(0, "pcie_0_phy_bcr (release)", GCC_PCIE_0_PHY_BCR, 0);
	sleep_ms(10);
	put(0, "pcie_phy_aux again", GCC_PCIE_PHY_AUX, rd(GCC(GCC_PCIE_PHY_AUX)) | 1u);
	put(0, "pcie0_phy_refgen again", GCC_PCIE_0_PHY_REFGEN, rd(GCC(GCC_PCIE_0_PHY_REFGEN)) | 1u);
	for (unsigned i = 0; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		wr(reg(2, phy_sequence[i].reg), phy_sequence[i].value);
	}
	sleep_ms(10);
	{
		uint32_t status = 0;

		for (uint32_t waited = 0; waited < 20; waited++) {
			status = rd(reg(2, PHY_STATUS));
			if (status & (1u << 6)) {
				break;
			}
			sleep_ms(10);
		}
		line("the PHY %s (status %#x, bit 6 is its ready)", (status & (1u << 6)) ? "IS READY" : "DID NOT BECOME READY",
		    status);
	}
	return true;
}

static bool
step_clocks_dump(void)
{
	/* the whole PCIe0 corner of the clock controller: the two RCGs' own registers and everything around them */
	static const struct {
		uint32_t offset;
		const char *name;
	} area[] = {
		{ 0x6b000, "pcie_0_bcr" },
		{ 0x6b004, "pcie_0_gdscr" },
		{ 0x6b010, "q2a_axi_halt" },
		{ 0x6b014, "slv_axi_halt" },
		{ 0x6b01c, "mstr_axi_halt" },
		{ 0x6b024, "cfg_ahb_halt" },
		{ 0x6b028, "aux_halt" },
		{ 0x6b02c, "pipe_halt" },
		{ 0x6b038, "aux_clk_src rcgr" },
		{ 0x6b03c, "aux_clk_src cfg" },
		{ 0x6b040, "aux_clk_src m" },
		{ 0x6b044, "aux_clk_src n" },
		{ 0x6b048, "aux_clk_src d" },
		{ 0x6c000, "pcie_0_link_down area" },
		{ 0x6c01c, "pcie_0_phy_bcr" },
		{ 0x6f000, "pcie_phy_bcr" },
		{ 0x6f004, "phy_aux" },
		{ 0x6f00c, "pcie_phy_cfg_ahb_bcr" },
		{ 0x6f010, "pcie_phy_com_bcr" },
		{ 0x6f014, "phy_refgen_src rcgr" },
		{ 0x6f018, "phy_refgen_src cfg" },
		{ 0x6f02c, "pcie0_phy_refgen" },
		{ 0x52008, "pcie_0_axi_misc" },
		{ 0x5200c, "pcie_0_axi_misc2" },
		{ 0x9000c, "aggre_noc_pcie_tbu" },
	};

	announce("read the whole PCIe0 corner of the clock controller: the two source generators' own registers (the aux "
	    "clock's and the PHY reference generator's), every branch and every reset around them");
	for (unsigned i = 0; i < sizeof(area) / sizeof(area[0]); i++) {
		get(0, area[i].name, area[i].offset);
	}
	return true;
}

static bool
step_aux_src(void)
{
	/*
	 * The aux clock's source generator (gcc-kona.c:399: gcc_pcie_0_aux_clk_src, 0x6b038) was found all zeros, and
	 * its RCGR register is a command register: bit 0 updates the configuration, bit 1 switches the root on. Its
	 * table (ftbl_gcc_pcie_0_aux_clk_src) is the crystal at 19.2 MHz through a divider of one -- source 0, and
	 * the divider field holds the divider minus one.
	 */
	announce("switch the aux clock's own source on: the crystal through a divider of one, the root enable bit in its "
	    "command register, then see that its branches are no longer halted");
	put(0, "aux_clk_src cfg (source 0, divide by 1)", 0x6b03c, 0);
	put(0, "aux_clk_src m", 0x6b040, 0);
	put(0, "aux_clk_src n", 0x6b044, 0);
	put(0, "aux_clk_src d", 0x6b048, 0);
	put(0, "aux_clk_src root enable", 0x6b038, 0x2);
	sleep_ms(2);
	get(0, "aux_clk_src rcgr after", 0x6b038);
	get(0, "aux_halt after", GCC_PCIE_0_AUX_HALT);
	get(0, "cfg_ahb_halt after", GCC_PCIE_0_CFG_AHB_HALT);
	get(0, "mstr_axi_halt after", GCC_PCIE_0_MSTR_HALT);
	get(0, "slv_axi_halt after", GCC_PCIE_0_SLV_AXI_HALT);
	return true;
}

static bool
step_parf(void)
{
	announce("with power, clocks and the PHY awake, read the controller's own registers (before, a read here was a bus "
	    "that never returned)");
	get(1, "parf_sys_ctrl", 0x00);
	get(1, "parf_phy_ctrl", 0x40);
	get(1, "parf_phy_refclk", 0x4c);
	get(1, "parf_dbi_base (2.3.2, 0x168)", 0x168);
	/* The vendor driver (pci-msm.c:43) puts the DBI base at 0x350, not 0x168, and this block
	 * reads 0x358 non-zero while 0x16c reads zero. Which layout is live decides whether the
	 * zero written at 0x168 in step 9 ever reached the register that matters. */
	get(1, "parf_dbi_base (vendor, 0x350)", 0x350);
	get(1, "parf_slv_addr_space_size (2.3.2, 0x16c)", 0x16c);
	get(1, "parf_slv_addr_space_size (2.7.0, 0x358)", 0x358);
	get(1, "parf_q2a_flush", 0x1ac);
	get(1, "parf_ltssm", 0x1b0);
	get(1, "parf_bdf_translate", 0x24c);
	return true;
}

/*
 * What the vendor driver does to these registers between switching the clocks on and turning the link on
 * (qcom_pcie_init_2_3_2): the PHY's reset line out, the DBI base set to zero, the MAC power-down mux off, the
 * MHI clock reset, and the master's write-address halt. And the PHY's reference generator switched on first:
 * gcc_pcie0_phy_refgen_clk (0x6f02c) hangs off gcc_pcie_phy_refgen_clk_src (0x6f014), which was found with its
 * root switched off -- a branch with a stopped source produces no clock, and the PHY cannot lock without one.
 */
static bool
step_parf_init(void)
{
	uint32_t value;

	announce("the vendor driver's own register writes, and the PHY reference generator switched on");
	/*
	 * The core's own reset pulsed the way qcom_pcie_init_2_7_0 does it (that is the variant for this SoC in
	 * mainline), and then the word that tells the controller it is a root complex rather than an endpoint:
	 * PARF_DEVICE_TYPE, 0x1000 in the block, 4 for RC.
	 */
	put(0, "pcie_0_bcr (assert, the core's own reset)", GCC_PCIE_0_BCR, 1);
	sleep_ms(1);
	put(0, "pcie_0_bcr (release)", GCC_PCIE_0_BCR, 0);
	sleep_ms(2);
	get(0, "pcie_0_bcr after", GCC_PCIE_0_BCR);
	get(1, "parf_device_type before", 0x1000);
	put(1, "parf_device_type (root complex)", 0x1000, 4);
	get(1, "parf_device_type after", 0x1000);

	/*
	 * The PHY node asks for 100 MHz (GCC_PCIE0_PHY_REFGEN_CLK): GPLL0 divided by six.
	 * gcc-kona.c:462-465 selects parent 1 (gcc_parent_map_0) and clk-rcg2.c:36 puts
	 * the source in bits 8..10, so CFG_RCGR is 5 | (1 << 8) = 0x105. The old 0x501
	 * selected parent 5 with divider 1, neither of which this clock table defines.
	 */
	put(0, "phy_refgen_src cfg (source 1, divide by 6: 100 MHz)", 0x6f018, 0x105);
	put(0, "phy_refgen_src root enable", 0x6f014, 0x2);
	sleep_ms(2);
	get(0, "phy_refgen_src rcgr after", 0x6f014);
	get(0, "phy_refgen_src cfg after", 0x6f018);
	get(0, "pcie0_phy_refgen after", GCC_PCIE_0_PHY_REFGEN);

	value = get(1, "parf_phy_ctrl", 0x40);
	put(1, "parf_phy_ctrl (bit 0 out)", 0x40, value & ~1u);
	/* This SoC's 2.7.0 layout uses PCIE20_PARF_DBI_BASE_ADDR = 0x350 (pci-msm.c:43).
	 * 0x168 is the older 2.3.2 offset and is a different live register here; writing it
	 * corrupts the wrapper setup. */
	put(1, "parf_dbi_base (zero, vendor offset 0x350)", 0x350, 0);
	get(1, "parf_dbi_base_v1 (legacy offset, read only)", 0x168);
	/*
	 * Read the live register back before anything touches the window: if 0x350 does not
	 * hold its zero, the ELBI read in step 12 is not worth attempting and this round stops
	 * with a readable answer instead of a hang.
	 */
	value = get(1, "parf_dbi_base 0x350 read back", 0x350);
	/*
	 * The value written was 0, so 0 is what a register that accepted it reads back. Anything
	 * else -- including a non-zero base left from the bootloader -- means this offset is not
	 * the live one, and the ELBI read would be an unaimed access into a window whose base
	 * was never set. Stop here instead: the log then says which of the two offsets is real.
	 */
	if (value != 0) {
		line("0x350 did not take the zero write (reads %#x); the ELBI read is skipped", value);
		return false;
	}
	/*
	 * The controller's system control word as a later vendor variant of this init writes it: the master's and
	 * the slave's AXI clock gating off, and -- this one matters here -- the core clock's gating off too
	 * (CORE_CLK_CGC_DIS, bit 6), together with the L1 clock removal disable. The word was found with bit 6
	 * clear, and the core's own registers then never answered.
	 */
	value = get(1, "parf_sys_ctrl", 0x00);
	put(1, "parf_sys_ctrl (bit 29 out, core clock gating off)", 0x00,
	    (value & ~(1u << 29)) | (1u << 6) | (1u << 1));
	get(1, "parf_sys_ctrl read back", 0x00);
	/*
	 * The device tree's qcom,slv-addr-space-size is 0x4000000 (64 MiB), not the
	 * 0x10000000 the reference wrote: 0x358 is the register's own offset here, and
	 * the value has to be the size the DT names.
	 */
	put(1, "parf_slv_addr_space_size", 0x358, 0x4000000);
	get(1, "parf_slv_addr_space_size after", 0x358);
	/*
	 * The two NoC-side clocks the device tree lists besides the six in the vote (clock-names "tbu" and
	 * "ddrss_sf_tbu"): the first was found voted already, the second not.
	 */
	get(0, "aggre_noc_pcie_tbu", GCC_PCIE_TBU);
	put(0, "ddrss_pcie_sf_tbu (bit 0 in)", 0x8d058, rd(GCC(0x8d058)) | 1u);
	get(0, "ddrss_pcie_sf_tbu after", 0x8d058);

	value = get(1, "parf_mhi_clock_reset_ctrl", 0x174);
	put(1, "parf_mhi_clock_reset_ctrl (bit 4 in)", 0x174, value | (1u << 4));
	/* Written is not the same as took: read both back, so "did the write land" is a reading
	 * rather than an assumption. This is a separate question from the window probe above. */
	get(1, "parf_mhi_clock_reset_ctrl read back", 0x174);
	value = get(1, "parf_axi_mstr_wr_addr_halt_v2", 0x1a8);
	put(1, "parf_axi_mstr_wr_addr_halt_v2 (bit 31 in)", 0x1a8, value | (1u << 31));
	/* Read the clock vote register once more now that the PHY reference generator is running. Step 3
	 * votes CLOCK_VOTES and reads it back; bit 4 (pipe) is deliberately left out there because the pipe
	 * clock comes from the PHY. If it is still clear at the end of step 9, then the controller's AXI
	 * branches are not clocked when step 10 touches the IATU window, and a bus that never returns is
	 * what that predicts. Read only: nothing here changes a vote. */
	get(0, "pcie_0_axi_misc after the PHY reference generator", GCC_PCIE_0_AXI_MISC);
	/*
	 * The link's own state, read last and read only: step 8 took this before any of the writes
	 * above, so the two readings are what says whether the init moved it. 0 means the link never
	 * left detect; L0 is 0x11.
	 */
	get(1, "parf_ltssm after the init", 0x1b0);
	return true;
}

/*
 * The controller's own configuration space (the DBI window at 0x60000000) and the link control block at 0x60000f20.
 * The ELBI read is last: in the run before this one the log ended on exactly that access, so whatever comes before
 * it is what this step is for.
 */
static bool
step_dbi(void)
{
	announce("ask the controller's own three windows in the order that tells them apart: the address translator, the "
	    "configuration space the buses are seen through, the core's own 4 KB, and the link control block");
	/* One access only: the first one is the one that takes the board down, and knowing
	 * which of the six it is decides whether the mapping or the access rule is wrong. */
	get(4, "iatu_viewport", 0x00);
	return true;
}

/*
 * Step 10: the endpoint's own enable sequence, which this file has never done.
 *
 * The endpoint's two pads are in the NORTH TLMM tile: WLAN_EN gpio 20 at 0x0f914000 and
 * PERST# gpio 79 at 0x0f94f000 (pinctrl-kona.c:20-25, :1576; kona.dtsi:4894;
 * kona-pcie.dtsi:36). PERST# is active low; WLAN_EN is active high. The vendor's
 * cnss2/power.c raises WLAN_EN before the PCIe controller is powered, holds PERST asserted
 * while its clocks and PHY come up, and releases it only after ep_latency
 * (pci-msm.c:4567-4583). This step keeps the local diagnostic order but uses the same
 * pad values: gpio 20 pull-up drive 16 output-high, gpio 79 pull-down drive 2.
 *
 * The TLMM is a different block from the controller, so a wrong value here cannot wedge the PCIe
 * bus the way a wrong address inside 0x1c00000 can. Every register is read before it is written
 * and read back after, so a pad that does not take its value says so.
 */
static bool
step_endpoint(void)
{
	uint32_t cfg, io, wlan, perst;

	announce("drive the two endpoint pads this file has never touched: WLAN_EN (TLMM 20) high, then PERST# "
	    "(TLMM 79) released, in the vendor's order and with the device tree's pad settings");

	/* What the bootloader left on both pads, before any write. */
	get(7, "tlmm gpio20 cfg (wlan-en)", TLMM_CFG(GPIO_WLAN_EN));
	wlan = get(7, "tlmm gpio20 io (wlan-en)", TLMM_IO(GPIO_WLAN_EN));
	get(7, "tlmm gpio79 cfg (perst)", TLMM_CFG(GPIO_PERST));
	perst = get(7, "tlmm gpio79 io (perst)", TLMM_IO(GPIO_PERST));
	line("before: wlan-en io %#x (out %u), perst io %#x (out %u)", wlan,
	    (wlan & TLMM_OUT) ? 1u : 0u, perst, (perst & TLMM_OUT) ? 1u : 0u);

	/* WLAN_EN: set the low level first, then make it a GPIO output, then drive high.
	 * Configuring OE before the level can briefly drive the radio's enable low. */
	cfg = TLMM_FUNC_GPIO | TLMM_PULL_UP | TLMM_DRIVE(16) | TLMM_OE;
	put(7, "tlmm gpio20 io (low before output enable)", TLMM_IO(GPIO_WLAN_EN),
	    rd(reg(7, TLMM_IO(GPIO_WLAN_EN))) & ~TLMM_OUT);
	put(7, "tlmm gpio20 cfg (func gpio, pull-up, drive 16, output)", TLMM_CFG(GPIO_WLAN_EN), cfg);
	put(7, "tlmm gpio20 io (drive high)", TLMM_IO(GPIO_WLAN_EN),
	    rd(reg(7, TLMM_IO(GPIO_WLAN_EN))) | TLMM_OUT);
	sleep_ms(1);                    /* cnss2: WLAN_ENABLE_DELAY, 1000 us */
	get(7, "tlmm gpio20 cfg after", TLMM_CFG(GPIO_WLAN_EN));
	io = get(7, "tlmm gpio20 io after", TLMM_IO(GPIO_WLAN_EN));
	line("WLAN_EN is %s (io %#x)", (io & TLMM_OUT) ? "HIGH" : "STILL LOW", io);

	/* PERST#: release means high. Set the level before making the pad an output. */
	cfg = TLMM_FUNC_GPIO | TLMM_PULL_DOWN | TLMM_DRIVE(2) | TLMM_OE;
	put(7, "tlmm gpio79 io (release level before output enable)", TLMM_IO(GPIO_PERST), TLMM_OUT);
	put(7, "tlmm gpio79 cfg (func gpio, pull-down, drive 2, output)", TLMM_CFG(GPIO_PERST), cfg);
	get(7, "tlmm gpio79 io after release", TLMM_IO(GPIO_PERST));
	sleep_ms(2);                    /* vendor waits perst_delay_us, 1000..1005 us */
	get(7, "tlmm gpio79 cfg after", TLMM_CFG(GPIO_PERST));
	io = get(7, "tlmm gpio79 io after", TLMM_IO(GPIO_PERST));
	line("PERST# is %s (io %#x)", (io & TLMM_OUT) ? "RELEASED" : "STILL ASSERTED", io);

	/*
	 * The link's own state with the endpoint now powered and out of reset. This is the criterion:
	 * it read 0 (LTSSM_DETECT_QUIET) at step 8 and after step 9, and it can only leave detect if
	 * the endpoint is answering. Read, not written: PARF_LTSSM bit 8, which the vendor sets to
	 * start training (pci-msm.c:4409), is deliberately left alone, so a change here is
	 * attributable to the pads. If it stays 0, that bit is the next single variable.
	 */
	pause_group();
	get(1, "parf_ltssm after the endpoint enable", 0x1b0);
	return true;
}


/*
 * Step 11: vendor link training. pci-msm.c:4374-4409 writes the DBI training words
 * after PERST is released and before PARF_LTSSM bit 8; DBI accesses while PERST is
 * asserted can hang the bus. Step 10 already released it, so this step follows the
 * vendor order and then polls ELBI/DLLLA/ID together as pci-msm.c:4416-4424 does.
 */
static bool
step_train(void)
{
	announce("write the vendor's DBI training words, set PARF_LTSSM bit 8, and poll ELBI, DLLLA and "
	    "the endpoint ID for up to 100 ms");

	/* 0x080c: field 0x1f00 = 1; 0x0890: clear bit 0; then bracket the Gen3 preset. */
	uint32_t value = get(6, "dbi gen3_gen2_ctrl before", DWC_GEN3_GEN2_CTRL);
	put(6, "dbi gen3_gen2_ctrl (0x1f00 = 1)", DWC_GEN3_GEN2_CTRL, (value & ~0x1f00u) | (1u << 8));
	value = get(6, "dwc gen3_eq_control before", DWC_GEN3_EQ_CONTROL);
	put(6, "dwc gen3_eq_control (bit 5 in)", DWC_GEN3_EQ_CONTROL, value | 0x20u);
	value = get(6, "dwc gen3_related before", DWC_GEN3_RELATED);
	put(6, "dwc gen3_related (bit 0 out)", DWC_GEN3_RELATED, value & ~1u);
	value = get(6, "dwc gen3_misc_control before", DWC_GEN3_MISC_CONTROL);
	put(6, "dwc gen3_misc_control (bit 0 in)", DWC_GEN3_MISC_CONTROL, value | 1u);
	put(6, "dwc gen3_spcie_cap (0x55555555)", DWC_GEN3_SPCIE_CAP, DWC_CORE_PRESET);
	put(6, "dwc gen3_misc_control (bit 0 out)", DWC_GEN3_MISC_CONTROL, value & ~1u);
	value = get(6, "dwc device_control_status before", DWC_DEVICE_CONTROL_STTS);
	put(6, "dwc device_control_status (0x7000 = 5)", DWC_DEVICE_CONTROL_STTS,
	    (value & ~0x7000u) | (DWC_TLP_RD_SIZE << 12));

	put(1, "parf_ltssm (LTSSM_EN, bit 8 in)", 0x1b0,
	    get(1, "parf_ltssm", 0x1b0) | (1u << 8));
	/* The vendor's three checks: ELBI XMLH_LINK_UP (bit 10), DBI DLLLA (bit 29 of 0x80)
	 * and a real vendor/device word at DBI 0x00. One 100 ms bounded poll. */
	for (uint32_t waited = 0; waited < 20; ++waited) {
		const uint32_t stts = rd(reg(3, 0x08));
		const uint32_t dll = rd(reg(6, 0x80));
		const uint32_t id = rd(reg(6, 0x00));
		line("the link after %u ms: elbi %#x dll %#x id %#x", waited * 5, stts, dll, id);
		if ((stts & 0x400u) && (dll & (1u << 29)) && id != 0xffffffffu) {
			line("THE LINK IS UP after %u ms (endpoint %#04x:%#04x)", waited * 5,
			    id & 0xffffu, id >> 16);
			get(1, "parf_ltssm after", 0x1b0);
			return true;
		}
		sleep_ms(5);
	}
	line("THE LINK DID NOT COME UP in 100 ms");
	return false;
}

/*
 * Step 12: read the link control block, which is the only place the link state can be seen once
 * LTSSM_EN does not stick.
 *
 * Step 11 wrote PARF_LTSSM bit 8 and read back 0, so PARF's own state field cannot be the
 * criterion. The vendor does not read it either: after setting that bit it polls ELBI
 * (pci-msm.c:4416-4424, readl_relaxed(dev->elbi + PCIE20_ELBI_SYS_STTS) until XMLH_LINK_UP),
 * and reports the state as (val >> 12) & 0x3f. ELBI_SYS_STTS is offset 0x08 (pci-msm.c:91) in
 * the elbi window at 0x60000f20, so its status word is at 0x60000f28.
 *
 * WHY THIS IS THE RISKY STEP, AND WHY IT IS ONLY A READ: ELBI (0x60000f20) and DBI
 * (0x60000000) are in the same 4 KB page (0x60000000), while iATU (0x60001000) is in the next
 * one. Step 13 -- the DBI step that has never run -- hangs the board on its FIRST access, and
 * that access is iATU, not this page. So reading here does not touch the page step 13 dies on,
 * and if this read also hangs, the fault is not "the DBI page" but something both pages share
 * (the master's write-address halt, or the slave address space size).
 *
 * One access only. If it hangs, the last line before it is already on the medium (risky()).
 */
static bool
step_elbi(void)
{
	uint32_t status;

	announce("read the link control block's status word, the signal the vendor polls after it sets LTSSM_EN; "
	    "this page (0x60000f28) is not the page the DBI step hangs on");

	get(3, "elbi_sys_ctrl", 0x04);
	status = get(3, "elbi_sys_stts (XMLH_LINK_UP is bit 10, state is bits 12-17)", 0x08);
	line("the link control block says: link %s, state %#x (%s)",
	    (status & 0x400) ? "UP" : "DOWN", (status >> 12) & 0x3f,
	    (status & 0x400) ? "XMLH_LINK_UP set" : "XMLH_LINK_UP clear, the link never came up");
	return true;
}

/*
 * Step 12: the seven dm_core writes the vendor makes before its first ELBI read.
 *
 * This file has never touched 0x60000000. Every state it can put the controller in has now been
 * tried -- collapsed (wifi6), powered (wifi7), powered with five clocks voted (wifi5 step 12) --
 * and the window failed in all of them. The vendor writes this window seven times and then reads
 * ELBI (pci-msm.c:4379-4406, then :4419), so the state it reads in is one this file has never
 * reached.
 *
 * Careful about what that does and does not mean. It does NOT prove the window is reachable here:
 * the vendor's writes succeed in the vendor's environment, which says the vendor reaches a state
 * we have not, not that we can. Two extrapolations are both wrong -- "the vendor does less than
 * us" and "the vendor can, so we can".
 *
 * The seven, in the vendor's order. msm_pcie_write_reg_field (pci-msm.c:1303-1314) is
 * read-modify-write: clear the mask, OR the value in at its shift.
 *   0x080c  field 0x1f00 = 1
 *   0x08a8  mask  0x20
 *   0x0890  mask BIT(0) = 0        (i.e. clear bit 0)
 *   0x08bc  mask BIT(0) = 1
 *   0x0154  whole register = 0x55555555
 *   0x08bc  mask BIT(0) = 0
 *   0x0078  field 0x7000 = 5       (set max tlp read size)
 * The vendor's other write, 0x70+PCI_EXP_LNKCTL2 for target_link_speed, is skipped: the board
 * has no max-link-speed in its device tree (checked against the boot DTB), so target_link_speed
 * is 0 and the vendor skips it too.
 *
 * ELBI is NOT read in this step. If these writes land and ELBI still fails, ELBI is its own
 * question; if the first write hangs, ELBI never mattered.
 */
static bool
step_dm_core(void)
{
	uint32_t v, probe = 0;

	/*
	 * pcie_probe=conf: read the configuration window (0x60100000) before touching dm_core.
	 *
	 * A separate switch on purpose. dm_core and iatu both hang on their first access, and both
	 * are inside 0x60000000..0x60002000; conf is a different window on the same controller.
	 * If conf reads, the failure is specific to that range. If conf hangs too, it is something
	 * broader -- every window except parf and phy -- and the range stops being the suspect.
	 *
	 * The vendor reads exactly this word to decide whether the endpoint's config space is up:
	 * readl(dev->conf) != PCIE_LINK_DOWN, where PCIE_LINK_DOWN is 0xffffffff
	 * (pci-msm.c:4611, :202). So a return of 0xffffffff here has the vendor's own meaning.
	 */
	PE_parse_boot_argn("pcie_probe", &probe, sizeof(probe));
	if (probe != 0) {
		uint32_t conf;

		announce("probe the configuration window (0x60100000) before dm_core, to say whether the failure "
		    "is this range or every window but parf and phy");
		conf = get(5, "conf_vendor_device (window probe)", 0x00);
		line("the configuration window returned %#x (%s)", conf,
		    conf == 0xffffffffu ? "0xffffffff -- PCIE_LINK_DOWN, the vendor's own \"not accessible\" value"
		                         : "not 0xffffffff -- the window answered, so the failure is specific to 0x60000000");
		return true;            /* stop here: the probe is the whole round */
	}

	announce("the seven dm_core writes the vendor makes before it reads ELBI; the first access this file "
	    "has ever made to 0x60000000");

	/* 0x080c: field 0x1f00 = 1 (read-modify-write, mask 0x1f00, value 1) */
	v = get(6, "dwc gen3_gen2_ctrl before", DWC_GEN3_GEN2_CTRL);
	put(6, "dwc gen3_gen2_ctrl (0x1f00 = 1)", DWC_GEN3_GEN2_CTRL, (v & ~0x1f00u) | (1u << 8));

	/* 0x08a8: mask 0x20 set */
	v = get(6, "dwc gen3_eq_control before", DWC_GEN3_EQ_CONTROL);
	put(6, "dwc gen3_eq_control (bit 5 in)", DWC_GEN3_EQ_CONTROL, v | 0x20u);

	/* 0x0890: mask BIT(0) cleared */
	v = get(6, "dwc gen3_related before", DWC_GEN3_RELATED);
	put(6, "dwc gen3_related (bit 0 out)", DWC_GEN3_RELATED, v & ~1u);

	/* 0x08bc: mask BIT(0) set, then cleared -- the vendor brackets the preset write with it */
	v = get(6, "dwc gen3_misc_control before", DWC_GEN3_MISC_CONTROL);
	put(6, "dwc gen3_misc_control (bit 0 in)", DWC_GEN3_MISC_CONTROL, v | 1u);

	/* 0x0154: the whole register, the default preset */
	get(6, "dwc gen3_spcie_cap before", DWC_GEN3_SPCIE_CAP);
	put(6, "dwc gen3_spcie_cap (0x55555555)", DWC_GEN3_SPCIE_CAP, DWC_CORE_PRESET);

	v = get(6, "dwc gen3_misc_control again", DWC_GEN3_MISC_CONTROL);
	put(6, "dwc gen3_misc_control (bit 0 out)", DWC_GEN3_MISC_CONTROL, v & ~1u);

	/* 0x0078: field 0x7000 = 5, the max tlp read size */
	v = get(6, "dwc device_control_status before", DWC_DEVICE_CONTROL_STTS);
	put(6, "dwc device_control_status (0x7000 = 5)", DWC_DEVICE_CONTROL_STTS,
	    (v & ~0x7000u) | (DWC_TLP_RD_SIZE << 12));

	/*
	 * Read all seven back. Three states, and the first one is the one that matters: if the
	 * FIRST access here had hung, this file would never have written 0x60000000 either, and
	 * that would say the window is unreachable in this environment rather than that we are
	 * missing a step -- a different conclusion with a different next move.
	 */
	get(6, "dwc gen3_gen2_ctrl after", DWC_GEN3_GEN2_CTRL);
	get(6, "dwc gen3_eq_control after", DWC_GEN3_EQ_CONTROL);
	get(6, "dwc gen3_related after", DWC_GEN3_RELATED);
	get(6, "dwc gen3_misc_control after", DWC_GEN3_MISC_CONTROL);
	get(6, "dwc gen3_spcie_cap after", DWC_GEN3_SPCIE_CAP);
	get(6, "dwc device_control_status after", DWC_DEVICE_CONTROL_STTS);
	line("the seven dm_core writes are done; step 13 reads ELBI next, as its own question");
	return true;
}

/* The command database, copied once. step_rails reads it, and the early vote needs the
 * cx.lvl address from it; neither maps the 128 KiB window twice. */
static uint8_t rail_cmd_db[GPB_CMD_DB_BYTES] __attribute__((aligned(8)));
static bool rail_cmd_db_loaded;

static bool
cmd_db_copy(void)
{
	vm_offset_t va;

	if (rail_cmd_db_loaded) {
		return true;
	}
	va = ml_io_map(GPB_CMD_DB_BASE, GPB_CMD_DB_BYTES);
	if (!va) {
		return false;
	}
	for (uint32_t i = 0; i < GPB_CMD_DB_BYTES / 4; i++) {
		const uint32_t v = ((volatile uint32_t *)va)[i];

		__builtin_memcpy(rail_cmd_db + 4 * i, &v, 4);
	}
	rail_cmd_db_loaded = gpb_le32(rail_cmd_db + 4) == GPB_CMD_DB_MAGIC;
	return rail_cmd_db_loaded;
}

static bool
rail_address(const char *name, uint32_t *address)
{
	struct gpb_cmd_db_entry e;

	if (!cmd_db_copy()) {
		return false;
	}
	__builtin_memset(&e, 0, sizeof e);
	if (gpb_cmd_db_find(rail_cmd_db, sizeof rail_cmd_db, name, &e) != 0) {
		return false;
	}
	*address = e.address;
	return true;
}

struct rail_cmd {
	uint32_t addr;
	uint32_t data;
	const char *what;
};

/*
 * Write one RPMh request with the vendor's TCS sequence (rpmh-rsc.c:324-359, :221-244):
 * command words, CMD_WAIT_FOR_CMPL, CMD_ENABLE, clear AMC, enable AMC, trigger. The
 * completion judge is every command's ISSUED|COMPL bits, the same bits the vendor ISR
 * checks at :283-290. The IRQ status is printed too, but it is not the judge.
 */
static bool
rpmh_write_commands(const struct rail_cmd *cmds, unsigned count, const char *label)
{
	uint32_t config, tcs_count, per_tcs, id, polls, status = 0, irq;
	volatile uint32_t *drv;
	bool complete = false;

	if (count == 0 || count > 16) {
		line("%s: bad command count %u", label, count);
		return false;
	}
	drv = (volatile uint32_t *)(uintptr_t)ml_io_map(RSC_DRV2_BASE, 0x10000);
	if (!drv) {
		line("%s: no memory to map the RSC DRV 2 at %#x", label, RSC_DRV2_BASE);
		return false;
	}
	config = drv[RSC_DRV_CONFIG / 4];
	tcs_count = (config >> 12) & 0x3f;
	per_tcs = (config >> 27) & 0x1f;
	line("%s: the RSC DRV 2 config reads %#x: %u TCS, %u commands each", label, config, tcs_count, per_tcs);
	if (tcs_count < 2 || per_tcs < count) {
		line("%s: the DRV has too few TCS or command slots; no vote", label);
		return false;
	}
	for (id = 0; id < 2; id++) {
		if (drv[(RSC_TCS_OFFSET + RSC_TCS_STRIDE * id + RSC_TCS_STATUS) / 4]) {
			break;
		}
	}
	if (id == 2) {
		line("%s: no idle TCS among the first two; no vote", label);
		return false;
	}
	line("%s: using TCS %u (status %#x)", label, id,
	    drv[(RSC_TCS_OFFSET + RSC_TCS_STRIDE * id + RSC_TCS_STATUS) / 4]);

#define RSC_W(off, val) do { drv[(off) / 4] = (val); __asm__ volatile("dsb sy" ::: "memory"); } while (0)
#define TCS(reg) (RSC_TCS_OFFSET + RSC_TCS_STRIDE * id + (reg))

	for (polls = 0; polls < 2000; polls++) {
		if (drv[TCS(RSC_TCS_STATUS) / 4]) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	if (polls == 2000) {
		line("%s: TCS %u stayed busy; no vote", label, id);
		return false;
	}
	RSC_W(TCS(RSC_TCS_CMD_ENABLE), 0);
	RSC_W(TCS(RSC_TCS_CMD_WAIT), 0);
	RSC_W(TCS(RSC_TCS_CONTROL), drv[TCS(RSC_TCS_CONTROL) / 4] &
	    ~(RSC_CONTROL_TRIGGER | RSC_CONTROL_AMC_ON));
	RSC_W(TCS(RSC_TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		const uint32_t cmd = TCS(RSC_CMD_MSGID) + RSC_CMD_STRIDE * i;

		RSC_W(cmd, RSC_CMD_MSGID_WRITE_RESPONSE);
		RSC_W(cmd + (RSC_CMD_ADDR - RSC_CMD_MSGID), cmds[i].addr);
		RSC_W(cmd + (RSC_CMD_DATA - RSC_CMD_MSGID), cmds[i].data);
	}
	RSC_W(TCS(RSC_TCS_CMD_WAIT), (1u << count) - 1u);
	RSC_W(TCS(RSC_TCS_CMD_ENABLE), (1u << count) - 1u);
	{
		uint32_t control = drv[TCS(RSC_TCS_CONTROL) / 4];

		control &= ~RSC_CONTROL_TRIGGER;
		RSC_W(TCS(RSC_TCS_CONTROL), control);
		control &= ~RSC_CONTROL_AMC_ON;
		RSC_W(TCS(RSC_TCS_CONTROL), control);
		RSC_W(TCS(RSC_TCS_CONTROL), RSC_CONTROL_AMC_ON);
		RSC_W(TCS(RSC_TCS_CONTROL), RSC_CONTROL_AMC_ON | RSC_CONTROL_TRIGGER);
	}
	for (polls = 0; polls < 100000; polls++) {
		complete = true;
		for (unsigned i = 0; i < count; i++) {
			const uint32_t v = drv[(TCS(RSC_CMD_STATUS) + RSC_CMD_STRIDE * i) / 4];

			if (i == 0) {
				status = v;
			}
			complete = complete && v != 0xffffffffu && (v & 0x10100u) == 0x10100u;
		}
		if (complete) {
			break;
		}
		delay_for_interval(1, NSEC_PER_USEC);
	}
	irq = drv[TCS(RSC_TCS_IRQ_STATUS) / 4];
	RSC_W(TCS(RSC_TCS_CMD_WAIT), 0);
	RSC_W(TCS(RSC_TCS_CMD_ENABLE), 0);
	RSC_W(TCS(RSC_TCS_CONTROL), drv[TCS(RSC_TCS_CONTROL) / 4] &
	    ~(RSC_CONTROL_TRIGGER | RSC_CONTROL_AMC_ON));
	RSC_W(TCS(RSC_TCS_IRQ_CLEAR), 1);
#undef TCS
#undef RSC_W

	line("%s: the vote %s after %u polls; command status %#x, irq %#x", label,
	    complete ? "completed" : "DID NOT COMPLETE", polls, status, irq);
	return complete;
}

/*
 * pcie_power=1: vote the three rails the controller node names, before step 2 powers the
 * controller. pcie_power=2 prints the same sequence and writes nothing. No pcie_power means
 * no PMIC write. Values are from kona-pcie.dtsi:47-53 and the vendor rpmh-regulator map:
 * ldoa9 1.2 V, ldoa5 0.88 V, cx.lvl at NOM (256). The external same-board success does this
 * at its step 2; the local diagnostic vote is step 14 and is too late for the controller.
 */
static bool
early_power_vote(void)
{
	struct rail_cmd cmds[7];
	uint32_t mode = 0, cx = 0;
	unsigned n = 0;

	PE_parse_boot_argn("pcie_power", &mode, sizeof mode);
	if (mode == 0) {
		/* The command line has a 1023-byte limit; pp is the short form for one round. */
		PE_parse_boot_argn("pp", &mode, sizeof mode);
	}
	if (mode == 0) {
		return true;
	}
	if (mode > 2) {
		line("pcie_power=%u: use 1 to vote or 2 to print the sequence; no write", mode);
		return false;
	}
	if (!rail_address(RAIL_CX_NAME, &cx)) {
		line("%s is not in the command database; no early vote", RAIL_CX_NAME);
		return false;
	}

	cmds[n].addr = RAIL_LDOA9_ADDR + RAIL_VRM_VOLTAGE;
	cmds[n].data = RAIL_LDOA9_MV;
	cmds[n].what = "ldoa9 voltage 1200 mV";
	n++;
	cmds[n].addr = RAIL_LDOA9_ADDR + RAIL_VRM_ENABLE;
	cmds[n].data = RAIL_ENABLE_ON;
	cmds[n].what = "ldoa9 enable 1";
	n++;
	cmds[n].addr = RAIL_LDOA9_ADDR + RAIL_VRM_MODE;
	cmds[n].data = RAIL_MODE_PMIC5_LDO_HPM;
	cmds[n].what = "ldoa9 mode 7 (pmic5-ldo HPM)";
	n++;
	cmds[n].addr = RAIL_LDOA5_ADDR + RAIL_VRM_VOLTAGE;
	cmds[n].data = RAIL_LDOA5_MV;
	cmds[n].what = "ldoa5 voltage 880 mV";
	n++;
	cmds[n].addr = RAIL_LDOA5_ADDR + RAIL_VRM_ENABLE;
	cmds[n].data = RAIL_ENABLE_ON;
	cmds[n].what = "ldoa5 enable 1";
	n++;
	cmds[n].addr = RAIL_LDOA5_ADDR + RAIL_VRM_MODE;
	cmds[n].data = RAIL_MODE_PMIC5_LDO_HPM;
	cmds[n].what = "ldoa5 mode 7 (pmic5-ldo HPM)";
	n++;
	cmds[n].addr = cx;
	cmds[n].data = RAIL_CX_LEVEL;
	cmds[n].what = "cx.lvl level 256 (NOM)";
	n++;

	line("early power vote before the controller: %u commands for %s, ldoa9 and ldoa5", n, RAIL_CX_NAME);
	for (unsigned i = 0; i < n; i++) {
		line("early command %u: address %#x, data %u (%s)", i + 1, cmds[i].addr, cmds[i].data, cmds[i].what);
	}
	if (mode == 2) {
		line("pcie_power=2: dry run, no write made; pcie_power=1 makes these %u", n);
		return true;
	}
	if (!rpmh_write_commands(cmds, n, "early_power")) {
		return false;
	}
	sleep_ms(10);
	return true;
}

/*
 * Step 13: what the command database says about the two rails the vendor turns on.
 *
 * This answers a narrower question than "is the rail on". APSS cannot answer that: rpmh-regulator.c
 * get_voltage reads vreg->req.reg[], which is the kernel's own cache of what it asked for
 * (:1021-1030, :842-843), and is_enabled reads the same cache (:891). cmd_db_read_aux_data reads
 * the ARC level mapping, a constant table loaded once at init (:1277-1310). And PM8150 exposes no
 * LDO on SPMI at all -- its spmi-pmic node has pon, temp-alarm, clkdiv, sdam, gpio, rtc, adc5 and
 * adc-tm5, and no regulator -- so there is no SPMI back door either.
 *
 * What IS answerable, and is what this step asks:
 *   1. Does the command database know ldoa5 and ldoa9? Their addresses are the only way to vote
 *      them, and a missing entry means RPMh does not know these rails by those names.
 *   2. What does their aux data hold? For a VRM regulator the aux data is the ARC level mapping;
 *      its length says how many levels exist. This is the check that a voltage value written
 *      later is one the table actually has -- the one guard against writing a bad level.
 *
 * Names from the board's own DT: kona-regulators.dtsi gives qcom,resource-name "ldoa5" and
 * "ldoa9" (and the vendor driver uses that string directly, rpmh-regulator.c:1889 -- the mainline
 * driver would instead need qcom,pmic-id, which this board's DTB does not have at all).
 *
 * Read only. No vote is made here.
 */
static bool
step_rails(void)
{
	static const char *const rails[] = { "ldoa5", "ldoa9" };
	struct gpb_cmd_db_entry e;

	announce("ask the command database about ldoa5 and ldoa9, the two rails the vendor turns on before it "
	    "touches dm_core; APSS cannot read whether they are on, but it can read whether RPMh knows them");

	if (!cmd_db_copy()) {
		line("no command database or bad magic at %#llx", (unsigned long long)GPB_CMD_DB_BASE);
		return false;
	}
	/*
	 * The copy uses 32-bit reads: the database is mapped as device memory and byte accesses to it
	 * are not what GpuProbe uses either (GpuProbe.cpp:198-202).
	 */
	line("the command database at %#llx: version %#x magic %#x (expected magic %#x)",
	    (unsigned long long)GPB_CMD_DB_BASE, gpb_le32(rail_cmd_db), gpb_le32(rail_cmd_db + 4), GPB_CMD_DB_MAGIC);

	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		__builtin_memset(&e, 0, sizeof e);
		if (gpb_cmd_db_find(rail_cmd_db, sizeof rail_cmd_db, rails[i], &e) != 0) {
			line("%s: NOT IN THE COMMAND DATABASE -- RPMh does not know this rail by that name", rails[i]);
			continue;
		}
		/*
		 * The aux length is what says how many levels the rail's table has. A zero length means
		 * the entry exists but carries no mapping, which is the case a vote would have to be
		 * checked against before writing any level.
		 */
		/*
		 * The type is printed WITH the numbers on purpose. Reading "aux 1" without it invites
		 * exactly the mistake this round made: aux was taken for a one-entry level table, when
		 * these rails are VRM and rpmh-regulator.c only loads the aux level mapping for ARC
		 * (:1926-1928). The type comes from the board's own DT --
		 * kona-regulators.dtsi gives both nodes compatible "qcom,rpmh-vrm-regulator" and
		 * qcom,regulator-type "pmic5-ldo" -- and is written here as a literal because this file
		 * reads no device tree. A reading should carry the semantics that decide how to read it.
		 */
		line("%s: address %#x, type VRM (pmic5-ldo, DT), aux %u bytes -- aux is NOT a level table for VRM",
		    rails[i], e.address, e.aux_bytes);
		if (e.aux_bytes && e.aux) {
			char row[96];
			unsigned n = 0;

			n += snprintf(row + n, sizeof row - n, "%s: aux", rails[i]);
			for (unsigned k = 0; k + 3 < e.aux_bytes && n < sizeof row - 8; k += 4) {
				n += snprintf(row + n, sizeof row - n, " %u", gpb_le32(e.aux + k));
			}
			line("%s", row);
		}
	}
	line("step 13 is read only; no rail has been voted");
	return true;
}

/*
 * Step 14: turn ldoa5 on through RPMh -- the first write this file makes to the PMIC.
 *
 * Gated twice, because this is the first write to a power rail and a wrong value is not a
 * failed experiment, it is damaged hardware:
 *   pcie_rail=1  print the exact command sequence and make no write  (dry run)
 *   pcie_rail=2  make the writes
 * With no pcie_rail argument, step 14 does nothing at all.
 *
 * Why ldoa5 and not ldoa9 first: 0.9 V is the lower of the two, and the DT lists them as
 * separate rails (vreg-0p9 = pm8150_l5, vreg-1p8 = pm8150_l9), so turning one on is the
 * smallest experiment that can change the DWC window's reachability.
 *
 * The three values, each traced to the offset it is actually written to rather than to a
 * name that sounds right:
 *   +0  voltage  880 mV   DT pm8150_l5 min = max = 880000 uV; rpmh-regulator.c:986 divides by
 *                         1000, so the register takes millivolts
 *   +4  enable   1        RPMH_REGULATOR_REG_VRM_ENABLE, index 1 (rpmh-regulator.c:87-99)
 *   +8  mode     7        RPMH_REGULATOR_MODE_PMIC5_LDO_HPM (rpmh-regulator.c:281). This is
 *                         the pmic_mode FIELD, not the enum RPMH_REGULATOR_MODE_HPM which is 4;
 *                         set_mode_index writes the field (rpmh-regulator.c:1049-1050)
 *
 * These rails are VRM, not ARC (qcom,rpmh-vrm-regulator, pmic5-ldo), so rpmh-regulator.c
 * never loads an aux level mapping for them (:1926-1928) -- their aux word is not a level
 * table and must not be used as one.
 *
 * The RSC sequence follows GpuProbe's rpmhVoteDdr, which runs on this board every boot: the
 * v2 register offsets, DRV 2 at 0x18220000, TCS offset 0xd00, one idle TCS, IRQ enable, the
 * commands, cmd_enable, active mode then trigger, then wait for the completion bit.
 */
static bool
step_rail_vote(void)
{
	uint32_t mode = 0;
	struct rail_cmd cmds[3];

	PE_parse_boot_argn("pcie_rail", &mode, sizeof(mode));
	if (mode == 0) {
		return true;            /* not asked for: nothing happens, nothing is printed */
	}

	cmds[0].addr = RAIL_LDOA5_ADDR + RAIL_VRM_VOLTAGE;
	cmds[0].data = RAIL_LDOA5_MV;
	cmds[0].what = "voltage 880 mV";
	cmds[1].addr = RAIL_LDOA5_ADDR + RAIL_VRM_ENABLE;
	cmds[1].data = RAIL_ENABLE_ON;
	cmds[1].what = "enable 1";
	cmds[2].addr = RAIL_LDOA5_ADDR + RAIL_VRM_MODE;
	cmds[2].data = RAIL_MODE_PMIC5_LDO_HPM;
	cmds[2].what = "mode 7 (pmic5-ldo HPM)";

	announce("the first write this file makes to a power rail: ldoa5 (pm8150_l5, 0.9 V) through RPMh");
	/*
	 * Always print the sequence, in both modes. A dry run whose output has to be trusted from
	 * memory is not a check; this is the line a person reads before the write is allowed.
	 */
	for (unsigned i = 0; i < 3; i++) {
		line("command %u: address %#x, data %u (%s)", i + 1, cmds[i].addr, cmds[i].data, cmds[i].what);
	}
	if (mode != 2) {
		line("pcie_rail=%u: dry run, no write made; pcie_rail=2 makes these three", mode);
		return true;
	}
	if (!rpmh_write_commands(cmds, 3, "rail2")) {
		return false;
	}

	/*
	 * The criterion. This file has never made a dm_core access that returned, in any state it
	 * could produce. If this one returns, the rail was the reason -- and if it hangs, the rail
	 * is not, because after this vote it is certainly on.
	 */
	announce("read the DWC window, which is the whole point of the vote");
	{
		uint32_t v = get(6, "dwc window after the rail vote", DWC_GEN3_GEN2_CTRL);

		line("the DWC window returned %#x (%s)", v,
		    v == 0xffffffffu ? "all ones -- the bus answered with no data, so the rail was not it"
		                      : "a value -- the window answered, so the rail WAS the reason");
	}
	return true;
}

/* ---- what runs ------------------------------------------------------------------------------ */

static bool (*const steps[])(void) = {
	step_look,
	step_gdsc,
	step_clocks,
	step_phy,
	step_phy_seq,
	step_clocks_dump,
	step_aux_src,
	step_parf,
	step_parf_init,
	step_endpoint,
	step_train,
	step_rails,
	step_rail_vote,  /* gated by pcie_rail; no argument, no write */
	step_dm_core,
	step_elbi,
	step_dbi,
};

void
alioth_pcie_init(void)
{
	uint32_t steps_wanted = 0, skip = 0;

	PE_parse_boot_argn("pcie", &steps_wanted, sizeof(steps_wanted));
	if (steps_wanted == 0) {
		return;
	}
	risky();                /* everything the boot wrote before this is still in its ring: put it on the medium first */
	PE_parse_boot_argn("pcie_pause", &pcie_pause_ms, sizeof(pcie_pause_ms));
	PE_parse_boot_argn("pcie_skip", &skip, sizeof(skip));
	pcie_skip_steps = skip;
	if (steps_wanted > sizeof(steps) / sizeof(steps[0])) {
		steps_wanted = sizeof(steps) / sizeof(steps[0]);
	}
	line("the Wi-Fi's controller at 0x1c00000, steps up to %u, without those in mask %#x", steps_wanted, skip);
	/*
	 * A pause here, so that the loader (which writes the end of this log to storage every quarter of
	 * a second) has this line before any of the controller is touched: a step that hangs the machine
	 * takes whatever was not written down with it.
	 */
	sleep_ms(300);
	if (!map_all()) {
		return;
	}
	line("the blocks are mapped");
	pcie_last_step = steps_wanted;
	{
		uint32_t delay_ms = 0;

		/*
		 * pcie_delay=MS: wait first. The loader writes the end of this log down every quarter second for its
		 * first 24 seconds (then it hands the interrupt controller to the system), so a step that takes its time
		 * is best started well inside that window rather than in the first millisecond of the boot.
		 */
		PE_parse_boot_argn("pcie_delay", &delay_ms, sizeof(delay_ms));
		if (delay_ms) {
			printf("pcie: waiting %u ms before the steps\n", delay_ms);
			sleep_ms(delay_ms);
		}
	}
	{
		uint32_t threaded = 0;
		thread_t thread;

		/*
		 * pcie_thread=1: the steps run on a thread of their own instead of the one the system is booting
		 * on. An access to the controller that never returns then only costs that thread -- the boot goes
		 * on, the log is written out, and what it printed before the access is in it. On the boot's own
		 * processor the same access takes the whole log with it.
		 */
		PE_parse_boot_argn("pcie_thread", &threaded, sizeof(threaded));
		if (threaded == 0) {
			PE_parse_boot_argn("pt", &threaded, sizeof(threaded));
		}
		if (threaded) {
			kern_return_t started = kernel_thread_start(pcie_steps_thread, NULL, &thread);

			printf("pcie: pcie_thread=1: the thread to run the steps on %s (%d)\n",
			    started == KERN_SUCCESS ? "started" : "DID NOT START", started);
			if (started == KERN_SUCCESS) {
				uint32_t wait_ms = 3000;

				thread_deallocate(thread);
				/*
				 * The boot goes on without the steps, but not instantly: it reaches launchd, and a kernel with
				 * no system image panics there, some 300 ms after this -- which would cut the thread off after
				 * its first two accesses. So give it the time it needs first (pcie_wait=MS).
				 */
				PE_parse_boot_argn("pcie_wait", &wait_ms, sizeof(wait_ms));
				printf("pcie: letting the thread have %u ms before the boot goes on\n", wait_ms);
				sleep_ms(wait_ms);
				return;
			}
		}
	}
	run_steps();
}

static void
pcie_steps_thread(void *argument, wait_result_t wait_result)
{
	(void)argument;
	(void)wait_result;
	thread_set_thread_name(current_thread(), "alioth-pcie");
	if (!map_all()) {
		thread_terminate(current_thread());
	}

	run_steps();
	printf("pcie: the thread that ran the steps is done\n");
	thread_terminate(current_thread());
}

static void
run_steps(void)
{
	for (pcie_step = 1; pcie_step <= pcie_last_step; pcie_step++) {
		if (pcie_skip_steps & 1u << pcie_step) {
			continue;
		}
		if (!steps[pcie_step - 1]()) {
			line("step %u: FAILED; the steps after it are not attempted", pcie_step);
			return;
		}
		line("step %u: done", pcie_step);
	}
	line("steps done; the controller is left as it was found");
}
#endif /* ARM64_BOARD_CONFIG_SWEET */

/* ---- board shim ---------------------------------------------------------------------------- */
/* The driver asks the ios-sweet log to reach the medium before it touches the controller, so a
 * step that hangs the machine does not take the preceding lines with it. This board has no such
 * log; the persistent log writer owns that job here, and it flushes on its own cadence. */
int
sweet_log_flush(void)
{
	return 0;
}
