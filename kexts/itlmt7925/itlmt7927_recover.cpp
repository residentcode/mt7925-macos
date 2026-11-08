// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// itlmt7925 - MT7927: bring a card back that comes up dead at probe, so a restart is enough and nobody has to switch the
// power supply off.
//
// The MT7927 stays on standby power across a restart and a shutdown. Now and then it comes up wedged: driver ownership never
// comes back (CONN_ON_LPCTL stays 0x4 = OWN_SYNC after CLR_OWN), everything behind CONN_INFRA reads 0xdeadfeed (the bus
// timeout pattern of MediaTek connsys), and the Bluetooth half on USB does not enumerate either. That state survives PERST#,
// i.e. a warm restart (2026-11-08 15:13 -> 15:18), so upstream mt76, which just fails the probe with -ETIMEDOUT
// (mt7925_pci_probe -> __mt792xe_mcu_drv_pmctrl), leaves the card dead until standby power is removed.
//
// recover7927() runs on the worker thread when the probe ownership handshake fails. It escalates from harmless to invasive
// and stops at the first step after which ownership works and the chip id is readable again:
//   1. patience: the vendor driver's 2 s ownership budget instead of mt76's 0.5 s
//   2. force CONN_INFRA awake from the host CSR (WAKEPU_TOP), as MediaTek's own MT6639 driver does before a re-init
//   3. that driver's recovery reset of the Bluetooth and Wi-Fi subsystems through the CB_INFRA RGU (always-on CB_INFRA bus,
//      in BAR0 directly, not behind CONN_INFRA), then MCU ownership and the ROM code idle wait
//   4. secondary bus reset of the bridge above the card, only when the card is the only device behind it (once per boot)
// No PCIe FLR: on the MT7925 it leaves the chip broken until power is removed (Linux quirk_no_flr).
// Every read and result goes to m1-log, so a failure tells which domain is wedged.
// Boot-arg itlmt_revive=0 turns all of it off; itlmt_revive_test=1 runs every step once on a healthy card (to check they do
// no harm), then continues with the normal bring-up (=2 also resets the Bluetooth subsystem, which the running MtkBtFw then
// has to recover from).
#include "itlmt7925.hpp"
#include "mt7925_regs.h"

#include <string.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

#define DEADFEED 0xdeadfeedu

// Addresses from MediaTek's gen4m driver for the MT6639 (chips/mt6639/mt6639.c, include/chips/coda/mt6639/*.h), as BAR0
// offsets (CONN_HOST_CSR_TOP 0x7c060000 -> 0x0e0000, CONN_DBG_CTL 0x7c020000 -> 0x0d0000, CB_INFRA 0x70020000 -> 0x1f0000)
#define CONN_HOST_LPCTL          0x0e0010u      // 0x7c060010
#define CONN_HOST_MISC           0x0e00f0u      // 0x7c0600f0
#define CONN_HOST_WAKEPU_TOP     0x0e01a0u      // 0x7c0601a0 CONN_INFRA_WAKEPU_TOP: 1 = keep CONN_INFRA awake
#define CONN_HOST_PWR_STATES     0x0e0a10u      // 0x7c060a10 CONNSYS_PWR_STATES (read only, diagnostics)
#define CONN_CFG_IP_VERSION      0x7c011000u    // through CONN_INFRA: 0x03010001 / 0x03010002 when awake
#define CONN_DBG_AP2CONN_CLK     0x0d3000u      // 0x7c023000 (diagnostics)
#define CONN_DBG_BUS_TIMEOUT_IRQ 0x0d3400u      // 0x7c023400: bits 9:0 = 0x3ff means the bus hung (diagnostics)
#define CBINFRA_RGU_BT_RST       0x1f8610u      // 0x70028610 BT_SUBSYS_RST (Linux btmtk MTK_BT_RESET_REG_CONNV3)
#define CBINFRA_REMAP_1          0x1f6554u      // 0x70026554, set_cbinfra_remap(): 0x74037001
#define CBINFRA_REMAP_2          0x1f6558u      // 0x70026558: 0x70007000
#define CBINFRA_MCU_OWN          0x1f5030u      // 0x70025030 (read only, diagnostics)

static uint32_t gPcieResets = 0;                // secondary bus resets done this boot (a dead card must not reset the bus forever)
static int gReviveOn = -1, gReviveTest = 0;     // boot-args itlmt_revive (default 1) and itlmt_revive_test

static void reviveArgs()
{
    if (gReviveOn >= 0) return;
    uint32_t v = 1, t = 0;
    PE_parse_boot_argn("itlmt_revive", &v, sizeof(v));
    gReviveOn = v ? 1 : 0;
    if (PE_parse_boot_argn("itlmt_revive_test", &t, sizeof(t)) && t) gReviveTest = t >= 2 ? 2 : 1;
}

// itlmt_revive_test: true once per boot, for the first bring-up
bool itlmt7925::reviveTest7927()
{
    static bool done = false;
    reviveArgs();
    if (!gReviveOn || !gReviveTest || done) return false;
    done = true;
    return true;
}

static bool pcieCap(IOPCIDevice *d, uint8_t *cap)
{
    IOByteCount c = 0;
    if (!d || !d->extendedFindPCICapability(kIOPCICapabilityIDPCIExpress, &c) || !c || c > 0xc0) return false;
    *cap = (uint8_t)c;
    return true;
}

// The parts of config space a reset clears and that this driver or IOPCIFamily set up. MSI is not saved: the driver
// polls and never enables interrupts.
struct itl_cfg_save {
    uint32_t hdr[16];
    uint8_t cap;
    uint16_t devctl, lnkctl, devctl2, lnkctl2;
};

static void cfgSave(IOPCIDevice *d, itl_cfg_save &s)
{
    for (int i = 0; i < 16; i++) s.hdr[i] = d->configRead32((uint8_t)(i * 4));
    s.cap = 0;
    if (pcieCap(d, &s.cap)) {
        s.devctl = d->configRead16(s.cap + 0x08);
        s.lnkctl = d->configRead16(s.cap + 0x10);
        s.devctl2 = d->configRead16(s.cap + 0x28);
        s.lnkctl2 = d->configRead16(s.cap + 0x30);
    }
}

static void cfgRestore(IOPCIDevice *d, const itl_cfg_save &s)
{
    d->configWrite32(0x0c, s.hdr[3]);                          // cache line size, latency timer
    for (int i = 4; i <= 9; i++) d->configWrite32((uint8_t)(i * 4), s.hdr[i]);   // BAR0..5
    d->configWrite32(0x30, s.hdr[12]);                         // expansion ROM
    d->configWrite32(0x3c, s.hdr[15]);                         // interrupt line / pin
    if (s.cap) {
        d->configWrite16(s.cap + 0x08, s.devctl & 0x7fffu);    // never re-trigger FLR (bit 15)
        d->configWrite16(s.cap + 0x10, s.lnkctl & ~0x0020u);   // not "retrain link" (bit 5)
        d->configWrite16(s.cap + 0x28, s.devctl2);
        d->configWrite16(s.cap + 0x30, s.lnkctl2);
    }
    d->configWrite16(0x04, (uint16_t)(s.hdr[1] & 0xffffu));    // command last: memory decode on with the BARs in place
}

// After a reset the device answers config reads with 0xffff (or a CRS completion) until it is ready again.
static bool waitConfig(IOPCIDevice *d, uint16_t vid, uint32_t timeoutMs)
{
    for (uint32_t ms = 0; ms <= timeoutMs; ms += 10) {
        if (d->configRead16(0x00) == vid) return true;
        IOSleep(10);
    }
    return false;
}

// Ownership handshake as in the probe (fw_pmctrl, then drv_pmctrl), with the vendor driver's budget instead of mt76's
// 10 x 50 ms: CLR_OWN asked again every 200 ms for 2 s (gen4m include/pwr_mgt.h). Then the chip id through CONN_INFRA.
bool itlmt7925::alive7927(const char *tag)
{
    const uint32_t lpctl = 0x0e0000u + (MT_CONN_ON_LPCTL - 0x7c060000u);
    for (int i = 0; i < 10; i++) {
        wr(lpctl, PCIE_LPCR_HOST_SET_OWN);
        if (pollA(MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC, 4, 50)) break;
    }
    bool own = false;
    for (int r = 0; r < 10 && !own; r++) {
        wr(lpctl, PCIE_LPCR_HOST_CLR_OWN);
        for (int ms = 0; ms < 200 && !own; ms++) {
            if (!(rd(lpctl) & PCIE_LPCR_HOST_OWN_SYNC)) own = true;
            else IODelay(1000);
        }
    }
    uint32_t chip = rdA(MT_HW_CHIPID), lp = rd(lpctl);
    bool ok = own && chip != DEADFEED && chip != 0xffffffffu;
    m1log("revive %s: own %s chip=%08x lp=%08x -> %s", tag, own ? "ok" : "FAIL", chip, lp, ok ? "alive" : "dead");
    return ok;
}

// What each domain answers (read only). CONN_HOST_CSR (LPCTL, MISC, PWR_STATES), CB_INFRA and the debug block are in BAR0
// directly; IP version and CHIPID go through CONN_INFRA. 0xdeadfeed = the bus protection's answer for a domain that is
// asleep or hung (CONN_DBG_CTL DEADFEED_PROT_EN), 0xffffffff = the PCIe read itself failed.
void itlmt7925::dump7927(const char *tag)
{
    uint8_t cap = 0;
    uint16_t lnksta = pcieCap(pci, &cap) ? pci->configRead16(cap + 0x12) : 0xffff;
    m1log("revive %s: cfg %04x lnksta=%04x lp=%08x misc=%08x pwr=%08x wakepu=%08x", tag, pci->configRead16(0x00), lnksta,
          rd(CONN_HOST_LPCTL), rd(CONN_HOST_MISC), rd(CONN_HOST_PWR_STATES), rd(CONN_HOST_WAKEPU_TOP));
    m1log("revive %s: ipver=%08x chip=%08x rom=%08x busclk=%08x bustmo=%08x", tag, rdA(CONN_CFG_IP_VERSION),
          rdA(MT_HW_CHIPID), rd(MT7927_ROMCODE_INDEX), rd(CONN_DBG_AP2CONN_CLK), rd(CONN_DBG_BUS_TIMEOUT_IRQ));
    m1log("revive %s: cbinfra wf_rst=%08x bt_rst=%08x remap=%08x/%08x mcu_own=%08x", tag, rd(MT7927_CBINFRA_RGU_WF_RST),
          rd(CBINFRA_RGU_BT_RST), rd(CBINFRA_REMAP_1), rd(CBINFRA_REMAP_2), rd(CBINFRA_MCU_OWN));
}

// Step 2: force CONN_INFRA awake from the host side (CONN_HOST_CSR_TOP WAKEPU_TOP) and wait for its IP version to read.
// gen4m mt6639_mcu_reinit() does this before it resets anything. Leaves WAKEPU_TOP set; the caller clears it.
bool itlmt7925::wakeConninfra7927()
{
    wr(CONN_HOST_WAKEPU_TOP, 1u);
    uint32_t v = 0;
    for (int ms = 0; ms < 50; ms++) {
        v = rdA(CONN_CFG_IP_VERSION);
        if (v == 0x03010001u || v == 0x03010002u) { m1log("revive wake: conninfra up after %d ms, ipver=%08x", ms, v); return true; }
        IOSleep(1);
    }
    m1log("revive wake: conninfra did not answer, ipver=%08x", v);
    return false;
}

// Step 3: gen4m mt6639_mcu_reinit() + mt6639_mcu_init(): reset the Bluetooth and the Wi-Fi subsystem together through the
// CB_INFRA RGU (with WAKEPU_TOP still set), restore the CB_INFRA remap, give the MCU ownership and wait for the ROM code.
//   0x10351 = WHOLE_PATH_RST(0) | SUBSYS_RST(4) | bypass sleep protection (6, 16) | revert cycle 3 (15:8); 0x10340 releases it.
// The GPIO mux writes of the phone version are left out (board specific). bt=false resets the Wi-Fi subsystem only.
void itlmt7925::cbinfraReset7927(bool bt)
{
    if (rd(CBINFRA_REMAP_1) != 0x74037001u || rd(CBINFRA_REMAP_2) != 0x70007000u) {     // mt6639 set_cbinfra_remap()
        m1log("revive reset: cbinfra remap was %08x/%08x, restored", rd(CBINFRA_REMAP_1), rd(CBINFRA_REMAP_2));
        wr(CBINFRA_REMAP_1, 0x74037001u);
        wr(CBINFRA_REMAP_2, 0x70007000u);
    }
    if (bt) wr(CBINFRA_RGU_BT_RST, 0x10351u);
    wr(MT7927_CBINFRA_RGU_WF_RST, 0x10351u);
    IOSleep(10);
    if (bt) wr(CBINFRA_RGU_BT_RST, 0x10340u);
    wr(MT7927_CBINFRA_RGU_WF_RST, 0x10340u);
    IOSleep(50);
    wr(MT7927_CBINFRA_MCU_OWN_SET, 1u);
    uint32_t rom = 0;
    int ms = 0;
    for (; ms < 1000; ms++) {
        rom = rd(MT7927_ROMCODE_INDEX) & 0xffffu;
        if (rom == MT7927_MCU_IDLE_VALUE) break;
        IOSleep(1);
    }
    m1log("revive reset: %s, wf_rst=%08x bt_rst=%08x rom=%04x after %d ms", bt ? "bt+wf" : "wf only",
          rd(MT7927_CBINFRA_RGU_WF_RST), rd(CBINFRA_RGU_BT_RST), rom, ms);
}

// Step 4: secondary bus reset (hot reset) through the Bridge Control register of the port above the card. Refused when
// anything else sits behind that port, or when the port is hot-plug capable (a link drop there is a surprise removal).
bool itlmt7925::sbr7927()
{
    IOService *ppb = pci->getProvider();                       // IOPCI2PCIBridge
    IOPCIDevice *br = ppb ? OSDynamicCast(IOPCIDevice, ppb->getProvider()) : nullptr;
    if (!br) { m1log("revive sbr: no parent bridge"); return false; }
    int devs = 0;
    if (OSIterator *it = ppb->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject()) if (OSDynamicCast(IOPCIDevice, o)) devs++;
        it->release();
    }
    if (devs != 1) { m1log("revive sbr: %d devices behind the bridge, skipped", devs); return false; }
    uint8_t bcap = 0;
    if (pcieCap(br, &bcap) && (br->configRead16(bcap + 0x02) & (1u << 8)) &&      // slot implemented
        (br->configRead32(bcap + 0x14) & (1u << 6))) {                               // hot-plug capable
        m1log("revive sbr: bridge slot is hot-plug capable, skipped");
        return false;
    }
    itl_cfg_save s;
    cfgSave(pci, s);
    pci->setBusMasterEnable(false);
    uint16_t bc = br->configRead16(0x3e);
    br->configWrite16(0x3e, bc | 0x0040u);                     // Secondary Bus Reset
    IOSleep(5);                                                // spec minimum 1 ms
    br->configWrite16(0x3e, bc & ~0x0040u);
    IOSleep(200);                                              // link training + 100 ms before config requests
    bool back = waitConfig(pci, (uint16_t)s.hdr[0], 2000);
    cfgRestore(pci, s);
    if (is7927) aspmDisable7927();
    m1log("revive sbr: bridge %u:%u.%u, config %s, lnksta=%04x", br->getBusNumber(), br->getDeviceNumber(),
          br->getFunctionNumber(), back ? "back" : "NOT back", bcap ? br->configRead16(bcap + 0x12) : 0xffff);
    return back;
}

// Returns true when the card answers again (ownership taken, chip id readable). Bounded: about 15 s in the worst case.
// test: run every step on a live card (itlmt_revive_test), see reviveTest7927().
bool itlmt7925::recover7927(bool test)
{
    reviveArgs();
    if (!gReviveOn) { m1log("revive: off (itlmt_revive=0)"); return false; }
    bool bt = !test || gReviveTest >= 2;
    m1log(test ? "revive: TEST run on a live card (itlmt_revive_test=%d)" : "revive: card dead at probe, trying to bring it back",
          gReviveTest);
    dump7927("before");

    // 1. patience: the vendor driver's 2 s ownership budget
    if (alive7927("wait") && !test) return true;

    // 2. force CONN_INFRA awake, ask for ownership again
    bool ok = false;
    wakeConninfra7927();
    ok = alive7927("wake");
    if (ok && !test) { wr(CONN_HOST_WAKEPU_TOP, 0); return true; }

    // 3. vendor subsystem reset (Bluetooth + Wi-Fi), still with CONN_INFRA held awake
    cbinfraReset7927(bt);
    wr(CONN_HOST_WAKEPU_TOP, 0);
    dump7927("after reset");
    ok = alive7927("reset");
    if (ok && !test) return true;

    // 4. hot reset of the link, once per boot. No FLR: the MT7925 advertises it but stays broken after one until power is
    // removed (Linux quirk_no_flr for 14c3:7925 and 14c3:0616); secondary bus reset is what recovers those.
    if (gPcieResets < 1) {
        gPcieResets++;
        sbr7927();
        dump7927("after sbr");
        ok = alive7927("sbr");
        if (!ok) {                                              // a fresh link may need the wake + reset again
            wakeConninfra7927();
            cbinfraReset7927(bt);
            wr(CONN_HOST_WAKEPU_TOP, 0);
            ok = alive7927("sbr+reset");
        }
        if (ok && !test) return true;
    } else {
        m1log("revive: link reset already used this boot");
    }
    if (test) {
        m1log("revive: TEST finished, card %s", ok ? "alive" : "DEAD");
        return ok;
    }
    m1log("revive: card still dead; only removing standby power will help (please report the lines above)");
    return false;
}
