// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// MediaTek MT7925 firmware, embedded into the kext (nothing can be read from disk in Recovery).
// Origin: linux-firmware mediatek/mt7925/, redistributable under the MediaTek licence in LICENCE.mediatek.
// Built from the repo root, so the .incbin paths are relative to it.
#define INCBIN(sym, file)                                  \
    __asm__(".section __DATA,__const\n"                    \
            ".globl _" #sym "\n"                           \
            ".balign 16\n"                                 \
            "_" #sym ":\n"                                 \
            ".incbin \"" file "\"\n"                       \
            ".globl _" #sym "_end\n"                       \
            "_" #sym "_end:\n"                             \
            ".byte 0\n"                                    \
            ".text\n")

INCBIN(itl_fw_patch, "fw/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin");
INCBIN(itl_fw_ram, "fw/WIFI_RAM_CODE_MT7925_1_1.bin");

// MT7927 (MT6639) firmware: linux-firmware mediatek/mt7927/, fetched by scripts/fetch-mt7927-fw.sh. scripts/build.sh defines
// ITL_HAVE_MT7927_FW when both files are in fw/; without them the symbols exist but are empty and M1 refuses an MT7927.
#ifdef ITL_HAVE_MT7927_FW
INCBIN(itl_fw7927_patch, "fw/WIFI_MT6639_PATCH_MCU_2_1_hdr.bin");
INCBIN(itl_fw7927_ram, "fw/WIFI_RAM_CODE_MT6639_2_1.bin");
const unsigned char itl_fw7927_present = 1;
#else
const unsigned char itl_fw7927_patch[1] = {0}, itl_fw7927_patch_end[1] = {0};
const unsigned char itl_fw7927_ram[1] = {0}, itl_fw7927_ram_end[1] = {0};
const unsigned char itl_fw7927_present = 0;
#endif
