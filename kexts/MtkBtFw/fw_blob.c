// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// MediaTek MT7925 Bluetooth firmware, embedded into the kext.
// Origin: linux-firmware mediatek/mt7925/BT_RAM_CODE_MT7925_1_1_hdr.bin, redistributable under fw/LICENCE.mediatek.
// Built from the repo root, so the .incbin path is relative to it.
__asm__(".section __DATA,__const\n"
        ".globl _mtk_bt_fw\n"
        ".balign 16\n"
        "_mtk_bt_fw:\n"
        ".incbin \"fw/BT_RAM_CODE_MT7925_1_1_hdr.bin\"\n"
        ".globl _mtk_bt_fw_end\n"
        "_mtk_bt_fw_end:\n"
        ".byte 0\n"
        ".text\n");

// MT7927 (MT6639): only when scripts/build.sh found fw/BT_RAM_CODE_MT6639_2_1_hdr.bin (ITL_HAVE_MT7927_BT_FW)
#ifdef ITL_HAVE_MT7927_BT_FW
__asm__(".section __DATA,__const\n"
        ".globl _mtk_bt_fw6639\n"
        ".balign 16\n"
        "_mtk_bt_fw6639:\n"
        ".incbin \"fw/BT_RAM_CODE_MT6639_2_1_hdr.bin\"\n"
        ".globl _mtk_bt_fw6639_end\n"
        "_mtk_bt_fw6639_end:\n"
        ".byte 0\n"
        ".text\n");
const unsigned char mtk_bt_fw6639_present = 1;
#else
const unsigned char mtk_bt_fw6639[1] = {0}, mtk_bt_fw6639_end[1] = {0};
const unsigned char mtk_bt_fw6639_present = 0;
#endif
