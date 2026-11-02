# Roadmap

Status 2026-10-26. ✅ done and verified on hardware · 🟡 done in code, not yet confirmed on hardware · 🔧 in progress · ⏳ planned ·
❌ not planned for now. Dates are when the work landed. The full hardware log for each round is in
[TEST-NATIVE-WIFI.md](TEST-NATIVE-WIFI.md). Nothing below is a promise.

## Done

### Core driver (`itlmt7925.kext`)

| | Feature | Landed | Notes |
|---|---|---|---|
| ✅ | Firmware download (patch + RAM image, embedded), WFDMA rings, MCU/UNI commands | 2026-10 | re-implemented from Linux `mt76`. Firmware `20260813` from linux-firmware |
| ✅ | MAC address from the card's firmware | 2026-10 | |
| ✅ | Active scan, 2.4 GHz 1-13 + 5 GHz 36-165 | 2026-10 | |
| ✅ | DFS channels (52-64, 100-144) | 2026-10-17 | joined ch 52 at 160 MHz on Tahoe |
| ✅ | Open-system auth, association, WPA2-PSK 4-way handshake, pairwise + group keys (CCMP) | 2026-10 | |
| ✅ | Up to 4 injected networks by priority (legacy mode) | 2026-10 | |
| ✅ | Speed ladder `itlmt_fast` 0-6: HT/VHT/HE, 160 MHz, 2×2, Block Ack, step down on a failed handshake | 2026-10-12 | full link rate (~2400 Mb/s PHY) |
| ✅ | A-MSDU receive off by default (it collapsed TCP to ~50 KB/s) | 2026-10-14 | 33-41 MB/s with it off |
| ✅ | RX ring 512, TX staging 256 slots (no TX drops) | 2026-10-14 | |
| ✅ | Link-loss recovery: chip reset + firmware reload + rejoin | 2026-10-14 | `recover #1` seen working on hardware. The ctl-triggered rejoin has its own bug (below) |
| ✅ | Cipher-aware key path: WPA/WPA2 mixed APs (TKIP group, CCMP pairwise), 512-bit TKIP PTK, 32-byte GTK | 2026-10-18 | fixes networks that associated but never got online |
| 🟡 | AP table 48 → 128, eviction keeps networks heard in the current scan | 2026-10-19 | fixes missing 2.4 GHz networks in crowded places |
| ✅ | Runtime control (`itl_*` keys via user client) and the diagnostics in the registry (`m3-log`, `stat-log`, pulse line) | 2026-10 | |

### Native front end (`itlmt7925Native.kext`)

| | Feature | Landed | Notes |
|---|---|---|---|
| ✅ | `IO80211Controller` + infrastructure interface on Sequoia 24H32 | 2026-10-14 | release `v1.1`. 28-36 MB/s, `networkQuality` 293 / 45 Mb/s |
| ✅ | Scan list, join from the Wi-Fi menu / System Settings, password from the keychain | 2026-10-14 | |
| ✅ | **Port to Tahoe 25G241**: headers regenerated from the 26.7.1 kernel collection, 5-arg `setLinkState`, MLO address array, 13 new protocol handlers | 2026-10-16 | |
| ✅ | Tahoe beacon message header 0x44 (scan list was garbled) | 2026-10-16 | |
| ✅ | IE budget in beacon messages (panic #1), CoreCapture pipes started (panic #2) | 2026-10-17 | |
| ✅ | Link-quality event 0x27, so the family's missed-beacon monitor stops dropping the link after 60 s | 2026-10-17 | |
| ✅ | `getNSS` ioctl, trace ring buffer | 2026-10-17 | Tahoe: 176.6 / 65.0 Mb/s, 0 drops |

## In progress

### AirDrop / AWDL (branch `dev/awdl`, boot-arg `itlmt_awdl=1`)

Full research notes and the phase plan: [ROADMAP-AWDL.md](ROADMAP-AWDL.md).

| | Phase | State |
|---|---|---|
| ✅ | 0-1: research and observation (25G241 disassembly, which ioctls the family sends) | 2026-10-17 |
| ✅ | 2: AWDL virtual interface (`awdl0`), `IO80211AWDLProtocol` subclass, `wifip2pd` adopts it, no panic | 2026-10-19 (this part is also on `main`) |
| ✅ | 3 (TX): AWDL ioctls let through, configure handshake, PSF sync frame sent at the 440 ms period, hopping over social channels 6/44/149 via ROC | 2026-10-20 / 21 |
| 🔧 | 3 (RX): firmware sniffer in the ROC window, AWDL action frames found and handed to `IO80211AWDLPeerManager::actionFrameInput`. The last two panics (fake buffer, then the static pool) are fixed with a dynamic Family buffer pool, **not yet tested on hardware** | 2026-10-22 |
| ⏳ | 4: synchronise and announce (election, availability windows) | |
| ⏳ | 5: data path and AirDrop (`_airdrop._tcp` over `awdl0`) | |
| ⏳ | 6: hardening, merge into `main` | |

### Bluetooth (`MtkBtFw`, `BtLeDlFix`; notes in [BLUETOOTH.md](BLUETOOTH.md))

| | Item | State |
|---|---|---|
| ✅ | Controller firmware download over USB, `bluetoothd` sees a controller | macOS 15 (24H32), 2026-10-11 |
| ✅ | Classic devices (headphones) connect; BLE mouse connects with `BtLeDlFix` | macOS 15, 2026-10-12 |
| ❓ | Everything above on macOS 26 Tahoe (`25G241`); `BtLeDlFix` byte pattern may need re-deriving | not tested |
| ⏳ | Keyboard pairing; re-pairing after a restart | not recorded |
| ⏳ | `bt-state = error` after a warm reboot with the firmware already running (seen once, not reproduced) | `MtkBtFw` should treat "no answer to the first PATCH_DWNLD query" differently from a failure |

### Known bugs being worked on

| Bug | State |
|---|---|
| A rejoin triggered by the runtime control goes silent 4 s after link-up (`postmortem silent 4s`), with both wcid paths; recovery then rejoins cleanly | cause not known. A `pulse` trace was added to tell "no beacons" apart from "beacons not parsed" |
| Some APs fail the 4-way handshake at level 6 but pass at level 3 | the ladder handles it within the attempt. Cause not known |
| 1400-byte pings to some WAN hosts lose ~70 % while TCP is fast (Sequoia measurement) | cause not known |

## Planned

| Priority | Item | Why / what it needs |
|---|---|---|
| ⏳ high | **Sleep / wake** verified in native mode | not exercised yet. Needs the power-state path and a rejoin after wake |
| ⏳ high | One release line for both Tahoe and Sequoia, or a clear build guard | today the native kext is per-build; a wrong build can panic |
| ⏳ high | Tahoe builds other than 25G241 | ABI dump + `check_kext_links.py` per build |
| ⏳ | **WPA3 / SAE** | SAE commit/confirm in the driver, PMF (802.11w) |
| ⏳ | Upload performance (65 Mb/s vs 176 Mb/s down) | TX path tuning; rate control is the firmware's |
| ⏳ | Root cause of the A-MSDU collapse, then A-MSDU back on | would raise throughput |
| ⏳ | Open networks | association without RSN |
| ⏳ | Regulatory domain from the country code (today fixed world domain `00`) | |
| ⏳ | Hardware reports from other MT7925 laptops | needs testers |
| ❌ | 6 GHz | needs a regulatory / 6 GHz scan path; not before the items above |
| ❌ | Enterprise (802.1X / EAP) | out of scope for now |
| ❌ | Wi-Fi 7 MLO / 320 MHz | the card supports it, macOS on these machines does not need it |

## How decisions are made

- The verified Wi-Fi path is never broken for an experiment. Experiments live behind a boot-arg (off by default) or on a `dev/*`
  branch, and are merged by pull request once tested on hardware.
- Facts are recorded per macOS build (`24H32`, `25G241`). Anything not measured is written as unknown ("chưa xác định").
- Clean-room: no GPL code (itlwm, AirportItlwm, OWL, OpenDrop). Apple-side facts come from read-only inspection of the macOS
  binaries with `tools/kc*.py`.
