// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// Portions derived from the Linux mt76 driver, Copyright (C) 2020, 2023 MediaTek Inc. (BSD-3-Clause-Clear)
// itlmt7925 - MT7927 (Filogic 380 / MT6639) support.
// The MT7927 runs the same connac3 firmware interface as the MT7925, so everything above the bring-up (UNI commands, scan,
// join, WPA2, data path) is shared. What differs is ported here from upstream mt76 (openwrt/mt76, merged 2026):
//   mt792x_dma.c   mt7927_sema_status_read, mt7927_wfsys_reset, mt7927_dma_prefetch_setup, mt7927_wfdma_setup
//   mt7925/pci.c   mt7927 PCI ids, ASPM off, mt7927_dma_layout (RX rings 4 / 6 / 7)
//   mt7925/init.c  mt7925_mcu_set_dbdc(true) after the firmware runs
//   mt7925/main.c  band_idx per band (2.4 GHz = 0, 5/6 GHz = 1) and mt7927_reconfig_band
// Nothing in this file runs on an MT7925 (every entry point checks is7927 or is only called when it is set).
#include "itlmt7925.hpp"
#include "mt7925_regs.h"

#include <string.h>
#include <libkern/libkern.h>

static inline void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

#define MCU_UNI_DEV_INFO_UPDATE 0x01
#define MCU_UNI_BSS_INFO_UPDATE 0x02

// mt76_pci_disable_aspm(): clear ASPM L0s/L1 in Link Control of the device and of its upstream port.
// Linux: "MT7927: ASPM L1 causes unreliable WFDMA register access".
static void aspmOff(IOPCIDevice *d, const char *who, uint16_t *before)
{
    if (!d) return;
    IOByteCount cap = 0;
    if (!d->extendedFindPCICapability(kIOPCICapabilityIDPCIExpress, &cap) || !cap) {
        IOLog("itlmt7925: %s has no PCIe capability, ASPM left alone\n", who);
        return;
    }
    uint16_t lnkctl = d->configRead16(cap + 0x10);
    if (before) *before = lnkctl;
    if (lnkctl & 0x3) d->configWrite16(cap + 0x10, (uint16_t)(lnkctl & ~0x3u));
}

void itlmt7925::aspmDisable7927()
{
    uint16_t devBefore = 0xffff, upBefore = 0xffff;
    IOPCIDevice *up = nullptr;
    if (IOService *bridge = pci->getProvider()) up = OSDynamicCast(IOPCIDevice, bridge->getProvider());
    // upstream port first, as Linux does (pcie_capability_clear_word on the parent, then on the device)
    aspmOff(up, "upstream port", &upBefore);
    aspmOff(pci, "MT7927", &devBefore);
    note("aspm off: dev lnkctl %04x up %04x", devBefore, upBefore, 0);
}

// mt7927_sema_status_read(): the semaphore status is read through a temporary PCIE2AP remap
void itlmt7925::sema7927(uint32_t addr)
{
    uint32_t remap = rd(MT7927_PCIE2AP_REMAP_WF_0_54);
    wr(MT7927_PCIE2AP_REMAP_WF_0_54, (remap & ~MT7927_PCIE2AP_REMAP_WF_0_54_MASK) | MT7927_PCIE2AP_REMAP_WF_0_54_VAL);
    IODelay(20);
    uint32_t v = rd(addr);
    wr(MT7927_PCIE2AP_REMAP_WF_0_54, remap);
    IODelay(20);
    m1log("mt7927 sema %x = %08x", addr, v);
}

// mt7927_wfsys_reset(): CBInfra RGU reset of the Wi-Fi subsystem, give the MCU ownership, wait for ROM code idle
int itlmt7925::wfsysReset7927()
{
    sema7927(MT7927_SEMA_OWN_STA);
    wr(MT7927_CBINFRA_RGU_WF_RST, rd(MT7927_CBINFRA_RGU_WF_RST) | MT7927_CBINFRA_RGU_WF_RST_WF_SUBSYS);
    IOSleep(2);
    wr(MT7927_CBINFRA_RGU_WF_RST, rd(MT7927_CBINFRA_RGU_WF_RST) & ~MT7927_CBINFRA_RGU_WF_RST_WF_SUBSYS);
    IOSleep(10);
    wr(MT7927_CBINFRA_MCU_OWN_SET, 1u);
    if (!pollA(MT7927_ROMCODE_INDEX, 0xffffu, MT7927_MCU_IDLE_VALUE, 200)) {
        m1log("mt7927 wfsys reset timeout ROMCODE_INDEX=%04x", rd(MT7927_ROMCODE_INDEX) & 0xffff);
        return -110;
    }
    sema7927(MT7927_SEMA_OWN_STA_REP);
    return 0;
}

#define PREFETCH(base, depth) (((uint32_t)(base) << 16) | (depth))

// mt7927_dma_prefetch_setup()
void itlmt7925::dmaPrefetch7927()
{
    wrA(MT_WFDMA_PREFETCH_CTRL, rdA(MT_WFDMA_PREFETCH_CTRL));
    wrA(MT_WFDMA_PREFETCH_CFG0, 0x660077);
    wrA(MT_WFDMA_PREFETCH_CFG1, 0x1100);
    wrA(MT_WFDMA_PREFETCH_CFG2, 0x30004f);
    wrA(MT_WFDMA_PREFETCH_CFG3, 0x542200);
    wrA(MT_WFDMA0_RX_RING_EXT_CTRL(4), PREFETCH(0x0000, 0x8));
    wrA(MT_WFDMA0_RX_RING_EXT_CTRL(6), PREFETCH(0x0080, 0x8));
    wrA(MT_WFDMA0_RX_RING_EXT_CTRL(7), PREFETCH(0x0100, 0x4));
    wrA(MT_WFDMA0_TX_RING_EXT_CTRL(16), PREFETCH(0x0140, 0x4));
    wrA(MT_WFDMA0_TX_RING_EXT_CTRL(15), PREFETCH(0x0180, 0x10));
    wrA(MT_WFDMA0_TX_RING_EXT_CTRL(0), PREFETCH(0x0280, 0x4));
}

// mt7927_wfdma_setup() (runs after TX/RX DMA are enabled)
void itlmt7925::wfdmaSetup7927()
{
    rmwA(MT_WFDMA0_GLO_CFG, 0, GLO_ADDR_EXT_EN | GLO_FW_DWLD_BYPASS_DMASHDL);
    rmwA(MT_WFDMA0_GLO_CFG, GLO_CSR_LBK_RX_Q_SEL_EN, 0);
    rmwA(MT_WFDMA0_GLO_CFG_EXT1, 1u << 28, 1u << 28);
    rmwA(MT_WFDMA0_INT_RX_PRI, 0, 0x0F00);
    rmwA(MT_WFDMA0_INT_TX_PRI, 0, 0x7F00);
}

// The Linux series restores FW_DWLD_BYPASS_DMASHDL in the power-management wake path; do the same after a driverOwn() retry.
void itlmt7925::wake7927()
{
    if (!is7927 || !txWm.n) return;
    rmwA(MT_WFDMA0_GLO_CFG, 0, GLO_ADDR_EXT_EN | GLO_FW_DWLD_BYPASS_DMASHDL);
}

// ---------------------------------------------------------------------------------------------
// Interface records with a band index (MT7925: 0xff = auto, MT7927: 0 or 1)
// ---------------------------------------------------------------------------------------------
int itlmt7925::devInfo(bool active)
{
    static uint8_t ev[512];
    uint32_t evl = 0;
    uint8_t dev[16];
    bzero(dev, sizeof(dev));
    dev[0] = 0;                      // omac_idx
    dev[1] = vifBand;                // band_idx
    put16(dev + 4, 0); put16(dev + 6, 12);   // DEV_INFO_ACTIVE
    dev[8] = active ? 1 : 0;
    dev[9] = 0;                      // link_idx
    memcpy(dev + 10, mac, 6);
    int rc = uniSend(MCU_UNI_DEV_INFO_UPDATE, dev, sizeof(dev), false, ev, sizeof(ev), &evl, 3000);
    m2log("dev_info active=%d band=%u rc=%d status=%u", active ? 1 : 0, vifBand, rc, evl >= 52 ? le32(ev + 48) : 0xdead);
    return rc;
}

int itlmt7925::bssBasicMin(bool active)
{
    static uint8_t ev[512];
    uint32_t evl = 0;
    uint8_t bss[4 + 32];
    bzero(bss, sizeof(bss));
    uint8_t *b = bss + 4;
    put16(b + 0, 0);                 // tag UNI_BSS_INFO_BASIC
    put16(b + 2, 32);                // len
    b[4] = active ? 1 : 0;           // active
    b[5] = 0;                        // omac_idx
    b[6] = 0;                        // hw_bss_idx
    b[7] = vifBand;                  // band_idx
    put32(b + 8, 0x10001);           // CONNECTION_INFRA_STA
    b[12] = 1;                       // conn_state
    b[13] = 0;                       // wmm_idx
    put16(b + 20, 19);               // bmc_tx_wlan_idx = WTBL_RESERVED
    put16(b + 26, 19);               // sta_idx
    b[31] = 0;                       // link_idx
    int rc = uniSend(MCU_UNI_BSS_INFO_UPDATE, bss, sizeof(bss), false, ev, sizeof(ev), &evl, 3000);
    m2log("bss_info active=%d band=%u rc=%d status=%u", active ? 1 : 0, vifBand, rc, evl >= 52 ? le32(ev + 48) : 0xdead);
    return rc;
}

// mt7925_mcu_set_dbdc(): hdr{pad[4]} + UNI_MBMC_SETTING{tag, len, mbmc_en, band, pad[2]}
int itlmt7925::setDbdc(bool enable)
{
    static uint8_t ev[512];
    uint32_t evl = 0;
    uint8_t req[4 + 8];
    bzero(req, sizeof(req));
    put16(req + 4, UNI_MBMC_SETTING);
    put16(req + 6, 8);
    req[8] = enable ? 1 : 0;
    req[9] = 0;                      // band: unused
    int rc = uniSend(MCU_UNI_SET_DBDC_PARMS, req, sizeof(req), false, ev, sizeof(ev), &evl, 3000);
    m2log("mt7927 dbdc=%d rc=%d status=%u", enable ? 1 : 0, rc, evl >= 52 ? le32(ev + 48) : 0xdead);
    return rc;
}

// mt7927_reconfig_band(): the interface is removed and added again with the band index of the channel it is about to use.
// Linux does it in assign_vif_chanctx; here it runs right before the BSS / STA records of a join are sent.
int itlmt7925::reconfigBand(uint8_t ch)
{
    if (!is7927) return 0;
    uint8_t nb = chBandIdx(ch);
    if (nb == vifBand) return 0;
    m2log("mt7927 band %u -> %u for ch %u", vifBand, nb, ch);
    bssBasicMin(false);
    devInfo(false);
    vifBand = nb;
    int rc = devInfo(true);
    if (!rc) rc = bssBasicMin(true);
    return rc;
}

// RX ring 7: on the MT7927 the second MCU ring (Linux: q_rx[MT_RXQ_MCU_WA]). Frames go to the frame parser, everything else
// (TXFREE, events) to the same handler as ring 6, so the ring never fills up and stalls the DMA engine.
void itlmt7925::rxEvt2Poll()
{
    if (!rxEvt2.n) return;
    const uint8_t *e; uint32_t l; int bi;
    int guard = 0;
    while (guard++ < 64 && ringPeek(rxEvt2, &e, &l, &bi)) {
        if (l >= 16) {
            uint32_t d0 = le32(e), type = d0 >> 27, flag = (d0 >> 16) & 0xf;
            if (type == 2 || type == 8 || (type == 7 && flag == 1)) { rxPkts++; rxParseFrame(e, l); }
            else { evCount++; m3Event(e, l); }
        }
        ringRelease(rxEvt2, bi);
    }
}
