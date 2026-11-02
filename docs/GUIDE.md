# Install guide: MT7925 Wi-Fi on macOS

[Tiếng Việt](GUIDE.vi.md) · [README](../README.md) · [Roadmap](ROADMAP.md)

This guide takes you from an OpenCore Hackintosh with no working Wi-Fi to the MT7925 in the macOS Wi-Fi menu, then covers how
to check it, fix it and undo it. Read it through once before you start.

**Contents:** [1. Before you start](#1-before-you-start) · [2. Pick the release](#2-pick-the-release) · [3. Install the kexts](#3-install-the-kexts) ·
[4. Native mode](#4-native-mode-recommended) · [5. Legacy mode](#5-legacy-mode-ethernet-style) · [6. Check it works](#6-check-it-works) ·
[7. Other macOS builds](#7-other-macos-builds) · [8. Troubleshooting](#8-troubleshooting) · [9. Undo / go back](#9-undo--go-back) ·
[10. Report a problem](#10-report-a-problem) · [11. Build from source](#11-build-from-source) · [12. Boot-arg reference](#12-boot-arg-reference) · [13. Bluetooth](#13-bluetooth)

---

## 1. Before you start

**You need:**

- An x86_64 machine booting macOS through **OpenCore** with an MT7925 card. Check from Windows/Linux (`lspci -nn` shows
  `14c3:7925`) or from macOS (`ioreg -l | grep -i 7925`).
- **macOS 26 Tahoe** or **macOS 15 Sequoia** (see §2 for which build each release was checked on).
- **Another way online** for the first boot: USB Ethernet, Android USB tethering (HoRNDIS) or an iPhone over USB.
- A **backup of your EFI**, plus a way to edit the ESP if macOS does not boot (Windows, Linux, the OpenCore shell, or a USB
  stick with a second EFI).

**Remove** any other driver bound to this card from `EFI/OC/Kexts` and from `Kernel → Add`: older `itlmt7925` builds, and any
experimental MediaTek kext.

The kexts are loaded by OpenCore like any other injected kext. They do not need SIP changes, and nothing is installed into
`/Library/Extensions`.

## 2. Pick the release

| Your macOS | Release | Branch | Native kext built for |
|---|---|---|---|
| 26 Tahoe | **latest `v2.*`** | `main` | `25G241` (26.7.1) |
| 15 Sequoia | **`v1.1`** | `sequoia` | `24H32` (15.8.1) |

Check your build with `sw_vers -buildVersion`. If it differs from the table, read [§7](#7-other-macos-builds) before you
load the native kext. The core kext (`itlmt7925.kext`) uses no build-specific Apple ABI.

Each release has these files:

| File | What it is |
|---|---|
| `itlmt7925.kext.zip` | **core** driver. Always required. Its Wi-Fi credentials are empty |
| `itlmt7925Native.kext.zip` | **native front end**. Makes the card a real Wi-Fi interface. Does nothing without `itlmt_native=1` |
| `ItlWiFi.app.zip` | menu bar app for legacy mode (scan, join, speed test). Not needed in native mode |
| `SHA256SUMS.txt` | checksums. Verify with `shasum -a 256 -c SHA256SUMS.txt` |
| `LICENSE`, `LICENCE.mediatek` | licenses (the firmware inside the core kext is MediaTek's) |

## 3. Install the kexts

1. Mount the ESP: `sudo diskutil mount disk0s1` (or `scripts/esp-automount.sh`). Back it up:
   `cp -R /Volumes/EFI/EFI ~/EFI-backup-$(date +%Y%m%d)`.
2. Unzip both kexts into `EFI/OC/Kexts/`.
3. Add them to `config.plist → Kernel → Add`, **core first**:

   | BundlePath | ExecutablePath | PlistPath | MinKernel | MaxKernel | Enabled |
   |---|---|---|---|---|---|
   | `itlmt7925.kext` | `Contents/MacOS/itlmt7925` | `Contents/Info.plist` | | | `true` |
   | `itlmt7925Native.kext` | `Contents/MacOS/itlmt7925Native` | `Contents/Info.plist` | Tahoe build: `25.0.0` | Sequoia build: `24.99.99` | `true` |

   `Arch` = `x86_64` (or `Any`). The Min/MaxKernel values keep a native kext from loading on a macOS it was not built for,
   for example after a macOS upgrade. Leave them empty only if you know you will never boot the other macOS.
4. Run `ocvalidate` on the config (or `python3 tools/occonfig.py -c <config.plist> check`).

Then continue with **§4 (native, recommended)** or **§5 (legacy)**.

## 4. Native mode (recommended)

macOS sees a **Wi-Fi** interface: the Wi-Fi menu, System Settings → Wi-Fi, scanning and joining. The password stays in the
macOS keychain, not in the kext.

**Boot-args** (`NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args`, appended to what you already have):

```
itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1 itlmt_fast=6
```

With the helper in this repo (it writes a backup `config.plist.pre-native` next to the config):

```bash
C=/Volumes/EFI/EFI/OC/config.plist
python3 tools/occonfig.py -c $C args add --tag pre-native -- itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1 itlmt_fast=6
python3 tools/occonfig.py -c $C check
```

If you used legacy mode before, remove `itlmt_m1`, `itlmt_m2` and `itlmt_m3`: those are legacy mode's built-in join. Do
**not** add `itlmt_amsdu=1` (see [§8](#8-troubleshooting)).

If your boot-args also live in NVRAM (`nvram boot-args` prints them), NVRAM wins over `config.plist`. Reset NVRAM from the
OpenCore picker, or set `NVRAM → Delete` for `boot-args`.

**Reboot**, open the Wi-Fi menu, pick your network and enter its password.

Which networks join: WPA2-Personal (including WPA2/WPA3 "transition" networks through their WPA2 side) and WPA/WPA2 mixed
networks with a TKIP group cipher. These do **not** join: WPA3-only, open, enterprise and 6 GHz networks.

## 5. Legacy mode (Ethernet-style)

Use legacy mode on Sequoia if native mode does not work for you, or to get online during installation.

The card shows up as **Ethernet**: there is no Wi-Fi menu. The driver joins by itself after boot using networks stored **in plain
text** in the kext's `Info.plist`, up to 4 networks in priority order. Do not install `itlmt7925Native.kext`, or leave it
installed without `itlmt_native=1` (it then does nothing).

**Boot-args:** `itlmt_m2=1 itlmt_m3=1 itlmt_fast=6`.

**Inject your network.** On macOS:

```bash
P=EFI/OC/Kexts/itlmt7925.kext/Contents/Info.plist
/usr/libexec/PlistBuddy -c "Set :IOKitPersonalities:itlmt7925:itl_ssid 'HomeWifi'" \
                        -c "Set :IOKitPersonalities:itlmt7925:itl_psk  'my password'" "$P"
# more networks: itl_ssid2/itl_psk2 ... itl_ssid4/itl_psk4
```

On Windows, elevated PowerShell (reads the Wi-Fi profiles saved on that PC):

```powershell
.\scripts\inject-creds.ps1 -Kext 'E:\EFI\OC\Kexts\itlmt7925.kext' -Ssid 'HomeWifi','Phone-hotspot'
```

> [!CAUTION]
> An injected kext contains your Wi-Fi passwords in plain text. Never share it, never commit it, and never attach it to an
> issue.

After boot, **ItlWiFi.app** (from the release, or `scripts/build-itlwifi.sh --install`) gives you a menu bar list to scan and
join further networks, plus a speed test.

## 6. Check it works

```bash
sysctl -n kern.bootargs | tr ' ' '\n' | grep itlmt     # the boot-args actually in effect
kextstat | grep itlmt                                   # com.itlmt7925.kext always; com.itlmt7925.native in native mode
ioreg -rc itlmt7925 -d1 | grep wifi-                    # core: wifi-state = "connected", wifi-rate, channel, ...
ioreg -rc itlmt7925Wifi -d1 | head                      # native front end (native mode only)
networksetup -listallhardwareports | grep -A2 Wi-Fi     # native: a Wi-Fi port; legacy: Ethernet only
networkQuality -v                                       # speed
```

What you should see in native mode: a Wi-Fi port (`en<N>`) in `networksetup`, real network names in the Wi-Fi menu, and after
joining, `wifi-state = "connected"` with a rate around 2400 Mb/s at 160 MHz / 2×2.

## 7. Other macOS builds

The native front end implements Apple-private interfaces (`IO80211Controller`, `IO80211InfraInterface`, Skywalk). Their layout
can change between macOS builds, and a mismatch causes **garbled scan lists or a kernel panic**. It has been checked only on:

- Tahoe **25G241** (macOS 26.7.1): `main` / `v2.*`
- Sequoia **24H32** (macOS 15.8.1, and developed on 15.6.1): `sequoia` / `v1.1`

On another build, first check the native kext against your kernel collection, on the Mac, from a clone of this repository:

```bash
KC=/System/Library/KernelCollections/BootKernelExtensions.kc python3 tools/check_kext_links.py \
    build/itlmt7925Native.kext build/itlmt7925.kext
```

Exit code 0 with only the known baseline-missing symbols means the symbols link. This does **not** prove the struct layouts
match, so test the first boot with a tether ready. To port to a new build (dump the ABI, regenerate the headers, re-check
slots), see [IO80211-ABI.md](IO80211-ABI.md) and the "Tahoe (25G241) first load" section of
[TEST-NATIVE-WIFI.md](TEST-NATIVE-WIFI.md), then open an issue with the results.

## 8. Troubleshooting

| Symptom | What to do |
|---|---|
| No Wi-Fi port at all | `kextstat \| grep itlmt`. If `com.itlmt7925.native` is missing: check the Kernel → Add order (core first), check `MaxKernel` on the native entry (a `24.99.99` cap stops it loading on Tahoe), and check that the boot-args really are in effect (`sysctl kern.bootargs`; NVRAM overrides `config.plist`) |
| Native kext fails with `0xdc00800e` | the core kext's `Info.plist` lacks `OSBundleCompatibleVersion` (very old builds). Use the core kext from the same release |
| Wi-Fi menu shows garbled or empty names | ABI mismatch: the native kext does not match your macOS build (§7). Go back to legacy mode (§9) |
| Kernel panic naming `itlmt7925Native` / `IO80211` | boot with the native kext disabled (OpenCore picker → space → edit, or another EFI), then report it (§10) with the first 30 lines of the panic |
| Joins, then drops after about 60 s | fixed on Tahoe since round 27 (link-quality event). Make sure you run a current `v2.*` |
| A network keeps failing the password step | the driver steps down its speed level by itself when a 4-way handshake fails (`itlmt_fast` ladder). If one AP still fails, try `itlmt_fast=3` and report it. WPA3-only networks never join |
| 2.4 GHz networks missing from the list in a crowded area | fixed on `main` (AP table 48 → 128). Update |
| Very slow (around 50 KB/s) | `itlmt_amsdu=1` is set: remove it. A-MSDU receive collapses throughput on this driver (cause unknown) |
| Upload much slower than download | known: the TX path is less tuned than RX |
| Old `en*` Wi-Fi services after several attempts | System Settings → Network: remove the stale Wi-Fi service and add it again |
| After sleep/wake Wi-Fi is gone | sleep/wake is not verified yet. Toggle Wi-Fi off/on. If that does not help, reboot |

## 9. Undo / go back

**From native to legacy mode:**

```bash
C=/Volumes/EFI/EFI/OC/config.plist
python3 tools/occonfig.py -c $C args del --tag pre-legacy -- itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1
python3 tools/occonfig.py -c $C args add --tag pre-legacy2 -- itlmt_m2=1 itlmt_m3=1 itlmt_fast=6
python3 tools/occonfig.py -c $C check
```

Then inject your network (§5). The native kext can stay installed: it does nothing without `itlmt_native=1`.

**Remove the driver completely:** delete both entries from `Kernel → Add` and both kexts from `EFI/OC/Kexts`, then delete
every `itlmt_*` boot-arg.

**macOS does not boot:** from Windows/Linux/a USB EFI, copy `config.plist.<tag>` (the backup written by `occonfig.py`) or your
EFI backup over `config.plist`. If you used `scripts/native-wifi-test.sh install`, run `scripts/native-wifi-test.sh restore`.

## 10. Report a problem

Collect the logs:

```bash
scripts/native-wifi-test.sh logs     # writes ~/itlmt-backup/logs-<time>/
```

Open an issue with the bug report template and include: laptop model, `sw_vers`, release or commit, boot-args, which mode you
use, what you did, what happened, and the log folder (zipped). For a panic, add the first 30 lines of
`/Library/Logs/DiagnosticReports/*.panic`. **The logs contain network names (SSIDs/BSSIDs) and MAC addresses**: replace them
before you post.

## 11. Build from source

On macOS with the Xcode command line tools (`xcode-select --install`):

```bash
git clone https://github.com/thanhnndev/mt7925-macos && cd mt7925-macos
git checkout main                   # Tahoe; use "sequoia" for macOS 15
bash scripts/build.sh itlmt7925 itlmt7925Native     # -> build/*.kext (clones acidanthera/MacKernelSDK on first use)
bash scripts/build.sh MtkBtFw BtLeDlFix             # Bluetooth kexts (BtLeDlFix also clones acidanthera/Lilu)
bash scripts/build-itlwifi.sh                       # -> build/ItlWiFi.app
```

Host tests (any OS with a C++17 compiler):

```bash
cd test
c++ -std=c++17 -I../kexts/itlmt7925 wpa_test.cpp  ../kexts/itlmt7925/itl_wpa.cpp  -o wpa_test  && ./wpa_test
c++ -std=c++17 -I../kexts/itlmt7925 caps_test.cpp ../kexts/itlmt7925/itl_caps.cpp -o caps_test && ./caps_test
c++ -std=c++17 -I../kexts/itlmt7925 ba_test.cpp   ../kexts/itlmt7925/itl_ba.cpp   -o ba_test   && ./ba_test
```

For a test cycle on the Mac, `scripts/native-wifi-test.sh install build/itlmt7925Native.kext --stage b` backs up the ESP
(including the NVRAM boot-args), installs the kext and sets the boot-args. `restore` undoes it.

## 12. Boot-arg reference

| boot-arg | Mode | Meaning |
|---|---|---|
| `itlmt_native=1` | native | start the `IO80211Controller` front end |
| `itlmt_native_if=1` | native | publish the Wi-Fi interface |
| `itlmt_native_dp=1` | native | connect the interface to the data path. The core then scans and joins for macOS, and the legacy Ethernet front end stays off |
| `itlmt_fast=N` | both | highest data-path level, 0-6. **6** = HE, 160 MHz, 2×2, Block Ack. The driver steps down by itself if a join fails |
| `itlmt_m2=1 itlmt_m3=1` | legacy | built-in scan + join with the injected networks, and the data path |
| `itlmt_nopwr=1` | both, optional | do not send the regulatory power tables at join |
| `itlmt_no160=1` | both, optional | cap the bandwidth at 80 MHz |
| `itlmt_stat=1` | both, optional | publish `stat-log` / `rx-log` / `ts-log` in the registry for diagnosis |
| `itlmt_native_nvtrace=1` | native, optional | keep the start trace in NVRAM (diagnosis only) |
| `itlmt_amsdu=1` | debug only | accept A-MSDU in Block Ack. **Do not use**: throughput collapses |
| `itlmt_awdl=1` | experimental | enable the AWDL interface (`main`: the interface only, phase 2; `dev/awdl`: transmit + receive, phase 3). Not for daily use; `dev/awdl` may panic |

## 13. Bluetooth

The MT7925 is two devices: Wi-Fi on PCIe (everything above) and Bluetooth on USB (`0489:e11e` on the GA403WM; check yours with
`ioreg -p IOUSB -l | grep -i -B2 -A8 Wireless_Device`). The Bluetooth half is independent of the Wi-Fi kexts: you can install
either one without the other. Full investigation notes, status and open items: [BLUETOOTH.md](BLUETOOTH.md).

**Status:** checked on macOS 15 (`24H32`) only. The controller comes up, classic headphones connect, and a BLE mouse connects with
`BtLeDlFix`. **Not checked on macOS 26 Tahoe.** `BtLeDlFix` patches `bluetoothd` by byte pattern, so on a different macOS it may
simply do nothing (the BLE mouse then fails to connect again, nothing else breaks).

**Install:**

1. Get `MtkBtFw.kext` and `BtLeDlFix.kext` (from a release, or §11), and `BlueToolFixup.kext` from
   [acidanthera/BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM/releases) (2.7.2 was used). `BlueToolFixup` and `BtLeDlFix`
   need [Lilu](https://github.com/acidanthera/Lilu).
2. Copy them to `EFI/OC/Kexts` and add them to `Kernel → Add` after Lilu: `MtkBtFw.kext` (`Contents/MacOS/MtkBtFw`),
   `BlueToolFixup.kext`, `BtLeDlFix.kext`.
3. In `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82` set `bluetoothExternalDongleFailed` = `00` and
   `bluetoothInternalControllerInfo` = 14 bytes of `00`, and list both names under `NVRAM → Delete` for the same GUID, so OpenCore
   rewrites them every boot.
4. Reboot, then check:

```bash
ioreg -rc MtkBtFw -l | grep -E '"bt-state"|"bt-log"'      # bt-state = ready; bt-log lists every step
system_profiler SPBluetoothDataType | head -12             # a real address and chipset instead of the placeholder
```

**Undo:** set `Enabled` to false for `MtkBtFw.kext`, `BlueToolFixup.kext` (and `BtLeDlFix.kext` to undo only the BLE patch), or restore
your EFI backup. Boot-arg `-btledlfxoff` disables only `BtLeDlFix`.
