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
