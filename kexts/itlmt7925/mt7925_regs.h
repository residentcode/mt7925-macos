// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// Portions derived from the Linux mt76 driver, Copyright (C) 2020, 2023 MediaTek Inc. (BSD-3-Clause-Clear)
// Register constants for M0, taken from the Linux mt76 driver
// (drivers/net/wireless/mediatek/mt76/mt792x_regs.h, mt7925/regs.h, mt7925/pci.c),
// BSD-3-Clause-Clear, Copyright (C) MediaTek Inc.
#ifndef MT7925_REGS_H
#define MT7925_REGS_H

#include <stdint.h>

#define MT_PCI_VENDOR_ID        0x14c3
#define MT_PCI_DEVICE_MT7925    0x7925

#define MT_HW_CHIPID            0x70010200u
#define MT_HW_REV               0x70010204u

#define MT_CONN_ON_LPCTL        0x7c060010u
#define PCIE_LPCR_HOST_SET_OWN  (1u << 0)
#define PCIE_LPCR_HOST_CLR_OWN  (1u << 1)
#define PCIE_LPCR_HOST_OWN_SYNC (1u << 2)

// L1 remap window used for addresses outside the fixed map
#define MT_HIF_REMAP_L1         0x155024u
#define MT_HIF_REMAP_L1_MASK    0xffff0000u
#define MT_HIF_REMAP_BASE_L1    0x130000u

// BAR0 is 2 MiB; offsets below this are direct
#define MT_BAR0_DIRECT_LIMIT    0x200000u

// ---------------------------------------------------------------------------------------------
// M1: DMA rings, MCU transport, firmware download.
// Sources: mt792x_regs.h, mt7925/regs.h, mt792x_dma.c, mt7925/pci.c, mt76_connac_mcu.[ch], mt7925/mcu.c
// ---------------------------------------------------------------------------------------------
#define MT_HW_EMI_CTL              0x18011100u
#define MT_HW_EMI_CTL_SLPPROT_EN   (1u << 1)
#define MT_WFSYS_SW_RST_B          0x7c000140u
#define WFSYS_SW_RST_B             (1u << 0)
#define WFSYS_SW_INIT_DONE         (1u << 4)

#define MT_CONN_ON_MISC            0x7c0600f0u
#define MT_TOP_MISC_FW_STATE_MASK  0x7u
#define MT_TOP_MISC2_FW_PWR_ON     (1u << 0)
#define MT_TOP_MISC2_FW_N9_RDY     0x3u

#define MT7925_PCIE_MAC_INT_ENABLE 0x10188u
#define MT7925_PCIE_MAC_PM         0x10194u
#define MT_PCIE_MAC_PM_L0S_DIS     (1u << 8)

#define MT_WFDMA0_BASE             0xd4000u
#define MT_WFDMA0(o)               (MT_WFDMA0_BASE + (o))
#define MT_WFDMA0_RST              MT_WFDMA0(0x100)
#define MT_WFDMA0_RST_LOGIC_RST    (1u << 4)
#define MT_WFDMA0_RST_DMASHDL_ALL_RST (1u << 5)
#define MT_MCU_CMD                 MT_WFDMA0(0x1f0)
#define MT_MCU2HOST_SW_INT_ENA     MT_WFDMA0(0x1f4)
#define MT_MCU_CMD_WAKE_RX_PCIE    (1u << 0)
#define MT_WFDMA0_HOST_INT_STA     MT_WFDMA0(0x200)
#define MT_WFDMA0_HOST_INT_ENA     MT_WFDMA0(0x204)
#define MT_WFDMA0_GLO_CFG          MT_WFDMA0(0x208)
#define GLO_TX_DMA_EN              (1u << 0)
#define GLO_TX_DMA_BUSY            (1u << 1)
#define GLO_RX_DMA_EN              (1u << 2)
#define GLO_RX_DMA_BUSY            (1u << 3)
#define GLO_DMA_SIZE_3             (3u << 4)
#define GLO_TX_WB_DDONE            (1u << 6)
#define GLO_FIFO_DIS_CHECK         (1u << 11)
#define GLO_FIFO_LITTLE_ENDIAN     (1u << 12)
#define GLO_RX_WB_DDONE            (1u << 13)
#define GLO_CSR_DISP_BASE_PTR_CHAIN_EN (1u << 15)
#define GLO_OMIT_RX_INFO_PFET2     (1u << 21)
#define GLO_OMIT_RX_INFO           (1u << 27)
#define GLO_OMIT_TX_INFO           (1u << 28)
#define GLO_CLK_GAT_DIS            (1u << 30)
#define MT_WFDMA0_RST_DTX_PTR      MT_WFDMA0(0x20c)
#define MT_WFDMA0_RST_DRX_PTR      MT_WFDMA0(0x280)
#define MT_WFDMA0_INT_RX_PRI       MT_WFDMA0(0x298)
#define MT_WFDMA0_INT_TX_PRI       MT_WFDMA0(0x29c)
#define MT_WFDMA0_GLO_CFG_EXT0     MT_WFDMA0(0x2b0)
#define GLO_EXT0_CSR_TX_DMASHDL_EN (1u << 6)
#define MT_UWFDMA0_GLO_CFG_EXT1    MT_WFDMA0(0x2b4)
#define MT_WFDMA0_PRI_DLY_INT_CFG0 MT_WFDMA0(0x2f0)
#define MT_WFDMA0_TX_RING_EXT_CTRL(n) MT_WFDMA0(0x600 + 4 * (n))
#define MT_WFDMA0_RX_RING_EXT_CTRL(n) MT_WFDMA0(0x680 + 4 * (n))
#define MT_TX_RING_BASE            MT_WFDMA0(0x300)
#define MT_RX_EVENT_RING_BASE      MT_WFDMA0(0x500)
#define MT_RX_DATA_RING_BASE       MT_WFDMA0(0x500)
#define MT_WFDMA_DUMMY_CR          0x54000120u
#define MT_WFDMA_NEED_REINIT       (1u << 1)
#define MT_DMASHDL_SW_CONTROL      0x7c026004u
#define MT_DMASHDL_DMASHDL_BYPASS  (1u << 28)

// ring register block (per queue, 0x10 apart)
#define Q_DESC_BASE  0x0
#define Q_RING_SIZE  0x4
#define Q_CPU_IDX    0x8
#define Q_DMA_IDX    0xc

#define MT7925_TXQ_BAND0           0
#define MT7925_TXQ_MCU_WM          15
#define MT7925_TXQ_FWDL            16
#define MT7925_RXQ_MCU_WM          0
#define MT7925_RXQ_BAND0           2

// DMA descriptor (16 bytes)
#define MT_DMA_CTL_SD_LEN0(l)      (((uint32_t)(l) & 0x3fffu) << 16)
#define MT_DMA_CTL_LAST_SEC0       (1u << 30)
#define MT_DMA_CTL_DMA_DONE        (1u << 31)
#define MT_RX_BUF_SIZE             2048

// MCU command ids
#define MCU_CMD_TARGET_ADDRESS_LEN_REQ 0x01
#define MCU_CMD_FW_START_REQ       0x02
#define MCU_CMD_NIC_POWER_CTRL     0x04
#define MCU_CMD_PATCH_START_REQ    0x05
#define MCU_CMD_PATCH_FINISH_REQ   0x07
#define MCU_CMD_PATCH_SEM_CONTROL  0x10
#define MCU_CMD_FW_SCATTER         0xee
#define MCU_PKT_ID                 0xa0
#define MCU_Q_NA                   3

#define PATCH_SEM_RELEASE          0
#define PATCH_SEM_GET              1
#define PATCH_IS_DL                1
#define PATCH_NOT_DL_SEM_SUCCESS   2
#define PATCH_REL_SEM_SUCCESS      3

#define DL_MODE_ENCRYPT            (1u << 0)
#define DL_MODE_RESET_SEC_IV       (1u << 3)
#define DL_CONFIG_ENCRY_MODE_SEL   (1u << 6)
#define DL_MODE_NEED_RSP           (1u << 31)
#define FW_FEATURE_SET_ENCRYPT     (1u << 0)
#define FW_FEATURE_ENCRY_MODE      (1u << 4)
#define FW_FEATURE_OVERRIDE_ADDR   (1u << 5)
#define FW_FEATURE_NON_DL          (1u << 6)
#define FW_START_OVERRIDE          (1u << 0)

#define PATCH_SEC_TYPE_MASK        0xffffu
#define PATCH_SEC_TYPE_INFO        0x2u
#define PATCH_SEC_NOT_SUPPORT      0xffffffffu

// Sizes of the on-disk firmware structures
#define PATCH_HDR_SIZE             96
#define PATCH_SEC_SIZE             64
#define FW_TRAILER_SIZE            36
#define FW_REGION_SIZE             40

// ---------------------------------------------------------------------------------------------
// MT7927 (Filogic 380, an MT6639 variant), from upstream mt76 (openwrt/mt76, 2026-10):
//   mt792x_regs.h, mt7925/mt7925.h, mt7925/pci.c, mt792x_dma.c (mt7927_* functions)
// The chip shares the MT7925 firmware interface; the host side differs in the WFSYS reset, the RX ring numbers,
// the WFDMA prefetch layout and a few GLO_CFG bits. ASPM L1 must be off (unreliable WFDMA register access).
// ---------------------------------------------------------------------------------------------
#define MT_PCI_DEVICE_MT7927       0x7927
#define MT_PCI_DEVICE_MT6639       0x6639
#define MT_PCI_DEVICE_MT7927_2     0x0738

#define MT7927_RXQ_BAND0           4
#define MT7927_RXQ_MCU_WM          6
#define MT7927_RXQ_DATA2           7

#define MT7927_CBINFRA_RGU_WF_RST           0x1f8600u
#define MT7927_CBINFRA_RGU_WF_RST_WF_SUBSYS (1u << 4)
#define MT7927_CBINFRA_MCU_OWN_SET          0x1f5034u
#define MT7927_ROMCODE_INDEX                0xc1604u
#define MT7927_MCU_IDLE_VALUE               0x1d1eu
#define MT7927_PCIE2AP_REMAP_WF_0_54        0x21008u
#define MT7927_PCIE2AP_REMAP_WF_0_54_MASK   0xffffu
#define MT7927_PCIE2AP_REMAP_WF_0_54_VAL    0x00001807u
#define MT7927_SEMA_OWN_STA                 0x40000u
#define MT7927_SEMA_OWN_STA_REP             0x40400u

#define MT_WFDMA_EXT_CSR(o)        (0xd7000u + (o))
#define MT_WFDMA_PREFETCH_CTRL     MT_WFDMA_EXT_CSR(0x30)
#define MT_WFDMA_PREFETCH_CFG0     MT_WFDMA_EXT_CSR(0xf0)
#define MT_WFDMA_PREFETCH_CFG1     MT_WFDMA_EXT_CSR(0xf4)
#define MT_WFDMA_PREFETCH_CFG2     MT_WFDMA_EXT_CSR(0xf8)
#define MT_WFDMA_PREFETCH_CFG3     MT_WFDMA_EXT_CSR(0xfc)
#define MT_WFDMA0_GLO_CFG_EXT1     MT_WFDMA0(0x2b4)        // MT7927 uses the WFDMA0 copy, MT7925 the UWFDMA0 one
#define GLO_FW_DWLD_BYPASS_DMASHDL (1u << 9)
#define GLO_CSR_LBK_RX_Q_SEL_EN    (1u << 20)
#define GLO_ADDR_EXT_EN            (1u << 26)

#define MCU_UNI_SET_DBDC_PARMS     0x28
#define UNI_MBMC_SETTING           0

// Fixed BAR0 windows, from __mt7925_reg_addr() (default_fixed_map)
struct itl_reg_map { uint32_t phys, maps, size; };
static const struct itl_reg_map itl_fixed_map[] = {
    { 0x830c0000, 0x000000, 0x0001000 }, { 0x54000000, 0x002000, 0x0001000 },
    { 0x55000000, 0x003000, 0x0001000 }, { 0x56000000, 0x004000, 0x0001000 },
    { 0x57000000, 0x005000, 0x0001000 }, { 0x58000000, 0x006000, 0x0001000 },
    { 0x59000000, 0x007000, 0x0001000 }, { 0x820c0000, 0x008000, 0x0004000 },
    { 0x820c8000, 0x00c000, 0x0002000 }, { 0x820cc000, 0x00e000, 0x0002000 },
    { 0x74030000, 0x010000, 0x0001000 }, { 0x820e0000, 0x020000, 0x0000400 },
    { 0x820e1000, 0x020400, 0x0000200 }, { 0x820e2000, 0x020800, 0x0000400 },
    { 0x820e3000, 0x020c00, 0x0000400 }, { 0x820e4000, 0x021000, 0x0000400 },
    { 0x820e5000, 0x021400, 0x0000800 }, { 0x820ce000, 0x021c00, 0x0000200 },
    { 0x820e7000, 0x021e00, 0x0000200 }, { 0x820cf000, 0x022000, 0x0001000 },
    { 0x820e9000, 0x023400, 0x0000200 }, { 0x820ea000, 0x024000, 0x0000200 },
    { 0x820eb000, 0x024200, 0x0000400 }, { 0x820ec000, 0x024600, 0x0000200 },
    { 0x820ed000, 0x024800, 0x0000800 }, { 0x820ca000, 0x026000, 0x0002000 },
    { 0x820d0000, 0x030000, 0x0010000 }, { 0x40000000, 0x070000, 0x0010000 },
    { 0x00400000, 0x080000, 0x0010000 }, { 0x00410000, 0x090000, 0x0010000 },
    { 0x820f0000, 0x0a0000, 0x0000400 }, { 0x820f1000, 0x0a0600, 0x0000200 },
    { 0x820f2000, 0x0a0800, 0x0000400 }, { 0x820f3000, 0x0a0c00, 0x0000400 },
    { 0x820f4000, 0x0a1000, 0x0000400 }, { 0x820f5000, 0x0a1400, 0x0000800 },
    { 0x820f7000, 0x0a1e00, 0x0000200 }, { 0x820f9000, 0x0a3400, 0x0000200 },
    { 0x820fa000, 0x0a4000, 0x0000200 }, { 0x820fb000, 0x0a4200, 0x0000400 },
    { 0x820fc000, 0x0a4600, 0x0000200 }, { 0x820fd000, 0x0a4800, 0x0000800 },
    { 0x820c4000, 0x0a8000, 0x0004000 }, { 0x820b0000, 0x0ae000, 0x0001000 },
    { 0x80020000, 0x0b0000, 0x0010000 }, { 0x81020000, 0x0c0000, 0x0010000 },
    { 0x7c020000, 0x0d0000, 0x0010000 }, { 0x7c060000, 0x0e0000, 0x0010000 },
    { 0x7c000000, 0x0f0000, 0x0010000 }, { 0x70020000, 0x1f0000, 0x0010000 },
    { 0x7c500000, 0x060000, 0x0200000 },
};
#define ITL_FIXED_MAP_N (sizeof(itl_fixed_map) / sizeof(itl_fixed_map[0]))

#endif
