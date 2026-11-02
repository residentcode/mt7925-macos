# mt7925-macos

**Wi-Fi and Bluetooth drivers for the MediaTek MT7925 (Wi-Fi 7 card, PCI `14c3:7925` + USB `0489:e11e`) on macOS, x86 Hackintosh.**
The two halves of the card are separate devices, so there are two sets of kexts in one repository. **Wi-Fi:** on macOS 26 Tahoe and
15 Sequoia the card shows up as a real Wi-Fi interface (Wi-Fi menu, network list in System Settings, joining with a password; no
password goes into the kext). **Bluetooth:** the controller works and BLE devices connect, checked on macOS 15 only so far.

[Tiếng Việt](README.vi.md) · **[Install guide](docs/GUIDE.md)** · [Roadmap](docs/ROADMAP.md) · [Releases](https://github.com/thanhnndev/mt7925-macos/releases) · [Bluetooth](docs/BLUETOOTH.md) · [AirDrop / AWDL work](docs/ROADMAP-AWDL.md)

> [!WARNING]
> **Experimental, AI-written, tested on one laptop.** Almost all code and documentation was written by an AI (Claude), directed
> and tested by one person on one **ASUS ROG Zephyrus G14 2025 (GA403WM)**. Nobody has audited it. The kexts run in the kernel, so
> a bug can panic or hang the machine. Back up first and always keep another way online (USB Ethernet, USB tethering).
> No warranty, see [LICENSE](LICENSE).

## Status at a glance (2026-11-02)

| | macOS 26 Tahoe (`25G241`, 26.7.1) | macOS 15 Sequoia (`24H32`, 15.8.1) |
|---|---|---|
| **Native mode** (Wi-Fi menu, join from System Settings) | ✅ **works**. Download 176.6 Mb/s, upload 65.0 Mb/s (`networkQuality`), HE 160 MHz 2×2, DFS channels | ✅ works: 28-36 MB/s on one TCP stream, `networkQuality` 293 Mb/s down / 45 Mb/s up |
| **Legacy mode** (shows up as Ethernet, password injected into the kext) | not tested on Tahoe | ✅ stable, the longest-tested path |
| Where to get it | branch `main`, release **v2.x** | branch `sequoia`, release **v1.1** |
| **Bluetooth** (`MtkBtFw` + `BtLeDlFix`) | ❓ not checked on `25G241`. `BtLeDlFix` patches `bluetoothd` and its bytes were derived on 24H32, so expect it to need work | ✅ controller works; BLE mouse and classic headphones connect. Keyboard pairing not recorded |
| AirDrop / AWDL / Continuity | 🔧 in progress on branch `dev/awdl` (off by default) | ❌ |

The native kext is tied to the macOS build it was written against: the Tahoe build is checked only on `25G241` and the
Sequoia build only on `24H32`. On any other build, check it first (see [GUIDE §7](docs/GUIDE.md#7-other-macos-builds)) before loading it.

## What works

- **Firmware and chip bring-up:** the MediaTek firmware is embedded in the kext, and the MAC address is read from the card.
- **Scanning:** 2.4 GHz channels 1-13 and all of 5 GHz (36-165), including the DFS channels 52-64 and 100-144. The AP table
  holds up to 128 networks, so 2.4 GHz networks are not pushed out of the list in crowded places.
- **Security:** WPA2-PSK with CCMP, plus WPA/WPA2 mixed networks (CCMP pairwise, TKIP group). The 4-way handshake runs in the
  driver, and in native mode macOS supplies the key.
- **Speed:** a link ladder from level 0 to level 6 covers HT, VHT and HE, 160 MHz, 2 spatial streams and Block Ack. If a network
  fails the 4-way handshake at a high level, the driver steps down a level by itself.
- **Native macOS integration:** an `IO80211Controller` and a Skywalk interface. You get the Wi-Fi menu, the System Settings list,
  RSSI and rate, link-quality events (so macOS does not drop the link after 60 s), CoreCapture, and the NSS ioctl.
- **Recovery:** when the link is lost, the driver resets the chip, reloads the firmware and joins again.
- **ItlWiFi.app:** a menu bar app that scans, joins and runs a speed test, for the Ethernet-style legacy mode.
- **Bluetooth:** `MtkBtFw` downloads the MT7925 Bluetooth firmware over USB (the Linux `btmtk` procedure), so `bluetoothd` sees a real
  controller; `BtLeDlFix` (a Lilu plugin) lets BLE devices finish connecting. Details and the open items: [docs/BLUETOOTH.md](docs/BLUETOOTH.md).
- **Host tests in CI:** WPA2 crypto vectors, HT/VHT/HE capability parsing, the Block Ack reorder buffer, and a check that the
  generated stubs match the headers.

## What does not work yet

WPA3 / SAE, open networks, enterprise (802.1X), 6 GHz, AirDrop / AWDL (work in progress), and sleep/wake (not verified).
Bluetooth on Tahoe is unchecked, and a warm reboot once left `bt-state` at `error` although the controller worked (not reproduced).
Upload is slower than download. A-MSDU receive is off on purpose. Full list and plan: **[docs/ROADMAP.md](docs/ROADMAP.md)**.

## Quick start

1. Pick the release that matches your macOS (table above) from [Releases](https://github.com/thanhnndev/mt7925-macos/releases)
   and unzip `itlmt7925.kext` and `itlmt7925Native.kext`.
2. Copy both to `EFI/OC/Kexts` and add them to `Kernel → Add` in this order: **`itlmt7925.kext` first, then `itlmt7925Native.kext`**.
   Remove any other Wi-Fi kext for this card.
3. Add the boot-args `itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1 itlmt_fast=6`.
4. Reboot, then join your network from the Wi-Fi menu.
5. Bluetooth (optional): add `MtkBtFw.kext`, `BlueToolFixup.kext` and `BtLeDlFix.kext`, see [GUIDE §13](docs/GUIDE.md#13-bluetooth).

Every step with checks, troubleshooting and how to undo it: **[docs/GUIDE.md](docs/GUIDE.md)**. For Bluetooth add `MtkBtFw.kext`,
`BlueToolFixup.kext` and `BtLeDlFix.kext`: [GUIDE §13](docs/GUIDE.md#13-bluetooth).

## Branches and releases

| Branch | What it is |
|---|---|
| `main` | Current driver: macOS 26 Tahoe native mode plus the latest core fixes. Releases `v2.*` are built from here |
| `dev/awdl` | AirDrop / AWDL work (phase 3: transmit works; receive reaches Apple's peer manager). Only active with `itlmt_awdl=1`. May panic |
| `sequoia` | Frozen macOS 15 line, the source of `v1.0` / `v1.1` |

| Release | macOS | Contents |
|---|---|---|
| `v1.0` | 15 Sequoia | `itlmt7925.kext` (legacy mode only), `ItlWiFi.app`. Copied from `ga403wm-hackintosh` `v1` (Wi-Fi files only) |
| `v1.1` | 15 Sequoia | `itlmt7925.kext` + `itlmt7925Native.kext` (native front end, A-MSDU off), `ItlWiFi.app`. Copied from `ga403wm-hackintosh` `kext-v1.1` |
| `bt-v1.0` | 15 Sequoia | Bluetooth: `MtkBtFw.kext` + `BtLeDlFix.kext`, copied unchanged from `ga403wm-hackintosh` `kext-v1.1`. Untested on Tahoe |
| `v2.*` | 26 Tahoe | built by CI from `main` when a `v2*` tag is pushed |

## Hardware

Any x86 Mac or Hackintosh with an MT7925 card (PCI vendor/device `14c3:7925`) should match. Only the GA403WM has been tested,
so reports from other laptops are welcome: use the bug report template and attach `scripts/native-wifi-test.sh logs`.

The card is two devices: Wi-Fi on PCIe and Bluetooth on USB. That is why Wi-Fi and Bluetooth live in this repository, in
separate kexts. Bluetooth is checked on the GA403WM only (USB `0489:e11e`); other boards may use another USB id, see
[docs/BLUETOOTH.md](docs/BLUETOOTH.md).

## Repository layout

| Path | Contents |
|---|---|
| `kexts/itlmt7925/` | core driver: firmware download, DMA rings, MCU commands, scan/auth/assoc, WPA2, Block Ack, data path, legacy Ethernet front end |
| `kexts/itlmt7925Native/` | native front end: `IO80211Controller`, infrastructure interface, (AWDL interface on `dev/awdl`) |
| `kexts/MtkBtFw/` | Bluetooth: downloads the controller firmware over USB |
| `kexts/BtLeDlFix/` | Bluetooth: Lilu plugin so BLE devices connect through this controller |
| `macos/ItlWiFi/` | menu bar app for legacy mode |
| `fw/` | MediaTek Wi-Fi and Bluetooth firmware from linux-firmware (its own license, see `fw/README.md`) |
| `scripts/` | build, install and test helpers (`build.sh`, `native-wifi-test.sh`, `inject-creds.ps1`, ...) |
| `tools/` | ABI research tools (kernel collection readers, vtable/slot checkers, header generators) |
| `abi/native-wifi/` | IO80211 / Skywalk ABI dumps for `24H32` and `25G241` |
| `docs/` | guide, roadmap, design notes, the full hardware test log (`TEST-NATIVE-WIFI.md`) |
| `test/` | host unit tests |

## History

This driver was developed inside [thanhnndev/ga403wm-hackintosh](https://github.com/thanhnndev/ga403wm-hackintosh) until
2026-10-26. The Wi-Fi part was moved here with its full commit history; the Bluetooth part was added on 2026-11-02 (its earlier
history stays in that repository). Older links and issues live there.

## License and credits

Code: [Clear BSD License](LICENSE). The MT7925 hardware layer is re-implemented from the Linux `mt76` driver (© MediaTek,
BSD-3-Clause-Clear). The Bluetooth download procedure follows the Linux `btmtk` driver. The firmware in `fw/` (Wi-Fi and Bluetooth) is under MediaTek's own license (`fw/LICENCE.mediatek`). The project is clean-room:
no code from `itlwm`, `AirportItlwm`, OWL or OpenDrop (all GPL). Apple-side layouts come from read-only inspection of
macOS binaries. Details: [NOTICE.md](NOTICE.md).
