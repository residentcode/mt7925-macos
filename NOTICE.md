# Third-party material

Author and maintainer: thanhnndev, <https://github.com/thanhnndev>. Everything in this repository is under the
[Clear BSD License](LICENSE) except the items below.

| Item | Where | Origin | License |
|---|---|---|---|
| MT7925 hardware layer (register map, WFDMA ring setup, MCU/UNI command framing, firmware download sequence, TXD/RXD layouts, STA_REC/BSS_INFO TLVs) | `kexts/itlmt7925/itlmt7925_m1.cpp`, `_m2.cpp`, `_m3.cpp`, `mt7925_regs.h` | Re-implemented from the Linux `mt76` driver (`drivers/net/wireless/mediatek/mt76/mt7925`, `mt76_connac_mcu.c`, `mt792x_*.c`), Copyright (C) 2020, 2023 MediaTek Inc. | BSD-3-Clause-Clear (SPDX header of each upstream file) |
| MT7925 Wi-Fi firmware (`WIFI_MT7925_PATCH_MCU_1_1_hdr.bin`, `WIFI_RAM_CODE_MT7925_1_1.bin`) | `fw/`, embedded into `itlmt7925.kext` | `linux-firmware`, `mediatek/mt7925/` (version and SHA-256 in `fw/README.md`) | MediaTek firmware license, `fw/LICENCE.mediatek`. Redistribution is permitted for use with devices containing MediaTek chipsets. **Not under the Clear BSD License.** |
| MT7925 Bluetooth firmware (`BT_RAM_CODE_MT7925_1_1_hdr.bin`) | `fw/`, embedded into `MtkBtFw.kext` | `linux-firmware`, `mediatek/` (version and SHA-256 in `fw/README.md`) | MediaTek firmware license, `fw/LICENCE.mediatek`. **Not under the Clear BSD License.** |
| Bluetooth firmware download procedure (WMT commands, section blocks) | `kexts/MtkBtFw/` | Follows the Linux `drivers/bluetooth/btmtk.c` procedure; re-written, not copied | see the SPDX header of the upstream file |
| Lilu headers and `plugin_start.cpp` | not in this repository | `scripts/build.sh BtLeDlFix` clones `acidanthera/Lilu` at build time; `BlueToolFixup.kext` comes from `acidanthera/BrcmPatchRAM` releases | their own licenses; check them before redistributing a built `BtLeDlFix.kext` |
| IO80211 / Skywalk class layouts | `kexts/itlmt7925/io80211/`, `kexts/itlmt7925Native/*_min.h`, `abi/native-wifi/` | Derived by read-only inspection of Apple's macOS kernel collections (`24H32`, `25G241`) with `tools/kc*.py` | facts about interfaces, no Apple code included |

## Clean room

No code from `itlwm`, `AirportItlwm`, OWL, OpenDrop or opendrop-awdl (all GPL) was read, copied or paraphrased. AWDL protocol
facts come from published papers and from Apple's own binaries.

## History

Developed in <https://github.com/thanhnndev/ga403wm-hackintosh> until 2026-10-26. The Wi-Fi part moved here with history; the Bluetooth part
(`MtkBtFw`, `BtLeDlFix`, `docs/BLUETOOTH.md`) was copied on 2026-11-02 and its earlier history stays there.
