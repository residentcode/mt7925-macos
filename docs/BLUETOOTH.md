# Bluetooth (MT7925, USB 0489:e11e)

> Moved into this repository on 2026-11-02 from `ga403wm-hackintosh`; the notes below were written there, so `fw/`, `kexts/` and `scripts/` paths are the same.
> All checks below were done on macOS 15 (24H32). Nothing here has been run on macOS 26 Tahoe yet. Install steps: [GUIDE §13](GUIDE.md#13-bluetooth).

The Bluetooth half of the MT7925 is a USB device (`Wireless_Device`, class ef, interfaces e0/01/01: ACL, SCO, ISO). It is a
standard HCI controller, but only after its RAM code has been downloaded; macOS has no loader for MediaTek parts, so
`bluetoothd` finds no controller (System Information shows a placeholder "BCM_4350C2, Address NULL, State Off").

> **Update 2026-10-12 (owner-confirmed):** the BLE mouse connects now, the `BtLeDlFix` plugin works; classic devices (headphones) work too.
> The sections below are the investigation in order. Still open: keyboard pairing was not recorded, and a warm reboot sets `bt-state` to
> `error` although the controller works.
>
> **Check 2026-10-12 22:1x, after a restart (not a cold boot):** `bt-state` = `ready`, `bt-log` = `chip 7925 fw_ver 00004200; firmware
> 20260813113236, 5 sections, 448367 bytes; section 0: already loaded; section 1: already loaded; section 2: 41472 bytes in 166 blocks;
> bluetooth function on (flag 0); ready in 1562 ms, 0 interrupt packets dropped`. So on this restart the controller kept sections 0-1 from
> before, the loader skipped them and reached `ready` in 1.6 s (cold boot: 5.2 s). The `error` after a warm reboot was **not**
> reproduced; one data point, so it stays open. Not measured in this session: re-pairing the BLE mouse, headphones and keyboard
> (they need a person and the devices).

> **Check 2026-11-03 23:49, after a restart (second data point, controller dead this time):** `bt-state` = `error`, `bt-log` =
> `chip 7925 fw_ver 00004200; ... section 0: already loaded; section 1: already loaded; wmt op 1 flag 2: no event; section 2: block
> 49 failed; failed in 7913 ms, 106 interrupt packets dropped`. Unlike the 2026-10-12 note the controller does **not** work:
> System Information shows the placeholder (BCM_4350C2, Address NULL, State Off). The chip stopped answering WMT events in the
> middle of section 2 (block 49 of 166), and the loader gives up on the first missing event: it has no retry and no
> subsystem reset (Linux `btmtk_usb_subsys_reset` / `btmtk_reset_sync`). The boot before it had AWDL active (heavy ROC on the
> shared radio, docs/ROADMAP-AWDL.md §10.29); whether that matters is not known. Recovery: a full shutdown (cold boot) reloads
> all sections.

## Status (2026-10-11, first boot with it)

Works as a controller: `bt-state` = ready after 5.2 s (sections 0-2 downloaded: 74 176 + 332 224 + 41 472 bytes, 0 dropped
interrupt packets), System Information shows `State: On`, `Chipset: THIRD_PARTY_DONGLE`, firmware `v9736 c4881` and a real
address. `bluetoothd` logs `addScanFilterByUUID failed 0x1812` (Apple's LE scan-filter offload, not present on non-Apple
controllers; the same message appears with Intel + BlueToolFixup). Pairing headphones / mouse / keyboard: not tested yet.

## BLE mouse does not connect (2026-10-12, UGREEN BLE Mouse, public address, HID over GATT)

Headphones (classic, A2DP/HFP) connect. The BLE mouse is not even paired: `bluetoothd` creates the LE link (status 0), reads
the remote version and features (`Remote Features: 21 ...`, remote and local both support the Data Length Extension) and then
stays in `LE_READY_WAIT_FOR_LENGTH` (4 of 4 attempts, `Le topology ... ready-state` in the unified log) until System Settings
gives up after about 30 s. No GATT discovery, no SMP, no pairing prompt.

Reading: after the features, Apple's LE stack sends LE Set Data Length and waits for the HCI *LE Data Length Change* event. The
Core spec only generates that event when the effective lengths change; a mouse that stays at 27 bytes gives none, and Broadcom
and Intel controllers evidently send one anyway, the MT7925 firmware does not. Linux's host stack does not wait for it, so
there is nothing in `btmtk.c` / `btusb.c` to port; the wait is inside `bluetoothd`. Not confirmed with an HCI trace.

How to check:
```
/usr/bin/log show --last 10m --style compact --predicate 'process == "bluetoothd"' | grep -E "Le topology|Remote Features"
```
(`zsh` has a `log` builtin: use `/usr/bin/log`.)

### What was done about it: `kexts/BtLeDlFix` (needs a reboot to test)

Disassembly of `/usr/sbin/bluetoothd` (x86_64 slice, 15.8.1 / 24H32; `lipo -thin x86_64`, `xcrun llvm-objdump -d`) shows the
LE connection setup computing a flag "both sides support Extended Data Length" at `0x1001e4f69`:

```
callq  <chipset id>          ; 0x1001e4f64
cmpl   $0x8, %eax            ; 0x1001e4f69   chipset 8 has a vendor quirk
movzbl %r14b, %r14d
movl   $0, %eax
cmovel %eax, %r14d           ; chipset == 8 -> flag 0 -> LENGTH step skipped
```

With the flag set, the state machine sends no command and waits for the Data Length Change event. `BtLeDlFix` is a Lilu
plugin (same `cs_validate_page` hook as `BlueToolFixup`) that rewrites `83 f8 08` to `39 c0 90` (`cmp eax,eax; nop`) in the
page of `bluetoothd` being validated, so the quirk applies to every controller. The 16-byte pattern
`83 f8 08 45 0f b6 f6 b8 00 00 00 00 44 0f 44 f0` occurs once in the binary. After an OS update the pattern may no longer
match: nothing breaks, the mouse simply fails again; look for `btledl` lines in `log show --predicate 'process == "kernel"'`
(`patched LE data length wait in /usr/sbin/bluetoothd` on success; `-btledlfxdbg` adds debug output) and re-derive the bytes.

Build: `scripts/build.sh BtLeDlFix` (clones acidanthera/Lilu into `build/Lilu` for the headers and `plugin_start.cpp`).
Boot-arg `-btledlfxoff` disables it. Untested on hardware at the time of writing.

If the mouse still stays in `LE_READY_WAIT_FOR_*`, the next step is an HCI trace (PacketLogger from Additional Tools for Xcode,
needs root) to see which command or event bluetoothd is waiting for.

Side note: this boot `bt-state` was `error` (`wmt op 1 flag 0: no event`: a warm reboot with the firmware already running),
yet the controller works; `MtkBtFw` should treat "no answer to the first PATCH_DWNLD query" differently from a real failure.

## LE advertising never starts (macOS 26.7.1 / 25G241, found 2026-11-04)

AirDrop needs the Mac's BLE AirDrop advertisement (docs/ROADMAP-AWDL.md §10.39, §10.41). `bluetoothd` scans fine but sends
no advertisement: every request ends in `[Server.LE.Advertise] Stack is not started, updateAdvertisement returning`.

**Cause (log of boot 2026-11-04 14:38 + disassembly of the x86_64 slice of `/usr/sbin/bluetoothd` 25G241).** No HCI command
is rejected by the controller. bluetoothd chooses the advertising path from its own chipset table:

* `[bm3_usb] Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE` stores chipset **0**.
* Right after `Stack Did Start` the advertise module calls `LE_InitMultipleAdvertising`. With BT 5.0 advertising sets off it
  takes the legacy multi-advertising path, which needs chipset ≠ 0 and a per-chipset vendor (Broadcom VSC) table; for
  chipset 0 it returns status 152: `LE_InitMultipleAdvertising failed with status STATUS 152`. The advertise module then
  stays `Idle stack:OFF` for the whole boot (81× `Stack is not ready, waiting for the stack to start`).
* The other path, BT 5.0 advertising sets (standard HCI LE Extended Advertising commands), is enabled by
  `BT chip support for BT Spec advertisingsets`. That answer comes from one chipset-number whitelist (true for chipsets
  0x13-0x19, 2001-3999 and ≥ 5000), so chipset 0 gets `support for BT 5.0 advertising sets is disabled` /
  `enableBTSpecAdvertisingSets=0`.

So for any third-party USB controller bluetoothd has no working advertising path. The MT7925 itself supports extended
advertising: its HCI supported-commands mask has octets 36-37 = `FF FF` (LE Set Advertising Set Random Address ... LE Clear
Advertising Sets) and the LE features (`FD FD FF FF BE ...`) have bit 12 (LE Extended Advertising) set.

**Fix: second patch in `kexts/BtLeDlFix`.** In the page of bluetoothd being validated, the one call site of that whitelist
(`__text+0x40fcb4`, the only `call [rcx+0x9c0]` in the binary) becomes `mov eax, 1; nop`:

```
48 8b 08 48 89 c7 ff 91 c0 09 00 00 41 89 c6   ->   48 8b 08 48 89 c7 b8 01 00 00 00 90 41 89 c6
```

The check before it (DeviceClass == "Mac") and the `DisableSpecSets` defaults override after it are unchanged. With spec
sets on, `LE_InitMultipleAdvertising` takes the advertising-sets path (no chipset table) and the advertise module can start.
Boot-arg `-btledlfxnoadv` turns only this patch off. Pattern unique in 25G241, inside one 4 KB page. After a macOS update the
pattern may not match: nothing breaks, the log simply shows `advertising sets is disabled` again; re-derive from the string
`BT chip support for BT Spec advertisingsets : %s` (its function's chipset call).

Side finding (corrected 2026-11-05): the 15.8.1 LE data length pattern does not occur in 25G241, but the same check exists
there in a different form (`cmp eax, 8; setne al; and al, r12b`, `__text+0x213915`). The build that was on the ESP at
14:38 / 17:30 (v1.1.0, backup `...-152640`) patched it; its source was never committed, so every build from this branch
(v1.0.0) dropped it. See "BLE mouse connected but dead" below.

Staged 2026-11-04 15:26 (backup `Kexts-backup/BtLeDlFix.kext.20261104-152640`).

**Result, cold boot 2026-11-04 15:52:** `BT Spec advertisingsets : enabled`, `enableBTSpecAdvertisingSets=1`, the advertise
module starts (`stack:ON`), but advertising flaps ~20×/s: in 5 min 6290× `Advertising failed to start with result 718` and
6287× `Advertising failed to stop with result 766`, so nothing stays on air.

**Second cause.** bluetoothd's `STATUS 7xx` is 700 + the HCI error code (`701` = 0x01 Unknown Command for VSC 0xFD3C).
* 718 = 0x12 *Invalid HCI Command Parameters*, from `multipleAdvStartCb`: the callback of command index 0xcf sent with
  (Enable 1, Num_Sets 1, handle, Duration 0, Max events 0), i.e. **LE Set Extended Advertising Enable**. The set being
  started advertises from a random address (`address: DB:9D:83:0E:07:AE type:1`). Core spec: Enable on a set whose own
  address type is random and whose random address was never set returns 0x12.
* The random address is written by `OI_HCI_CMD_LE_SetAdvertisingSetRandomAddress` (command index 0xcb, function
  `__text+0x1f713b`), which sends nothing (and returns success) unless a flag is 1. That flag defaults to 1 and initCoreStack
  clears it for chipset ≤ 3 (`cmp [chip+0x334], 3; ja; xor edi,edi; call set` at `__text+0x510e9e`). Chipset 0 → the
  address is never written → every Enable fails.
* 766 = 0x42 *Unknown Advertising Identifier*, from `setMultipleAdvertisingDataCompleteCb` right after `Halting advertising
  and resetting states`: the clean-up after the failed Enable removes the set, and the restart then writes data to it.
  A follow-on of 718.
* `Read LE Advertising TX power failed STATUS 712` (0x0C Command Disallowed) once at start: the legacy LE Read Advertising
  Physical Channel Tx Power after extended advertising commands, which the spec forbids mixing. Harmless (power 127 =
  unknown).

**Fix: third patch in `kexts/BtLeDlFix`:** the `call` that clears the flag becomes a 5-byte NOP, so the flag stays 1 and the
stack sends LE Set Advertising Set Random Address before Enable. The `chipset == 0` block right after it is unchanged.

```
83 b8 34 03 00 00 03 77 2a 31 ff e8 3e 6b ce ff   ->   ... 31 ff 0f 1f 44 00 00
```

Pattern unique in 25G241, inside one page. `-btledlfxnoadv` turns off both advertising patches. Staged 2026-11-04 16:07:
`/Volumes/ESP/EFI/OC/Kexts/BtLeDlFix.kext`; previous build (advertising sets only) in
`Kexts-backup/BtLeDlFix.kext.20261104-160722`. **Untested on hardware.**

**Result, boot 2026-11-04 16:52 (f7d0263), and revert.** 766 is gone but advertising still fails: 5578×
`Advertising failed to start with result 718` in 12 min (~9/s), one `Started advertising successfully`. The kernel log has
no `btledl` lines, so whether the third patch matched could not be confirmed. **Regression:** the BLE mouse connects
(`Outgoing LE Connection complete ... status 0`, encryption on, GATT connected) and shows Connected, but the pointer does
not move. The 14:38 boot without the advertising patches had a working mouse; the only difference seen in the log is the
continuous stream of failing LE extended advertising commands, so the flood is the prime suspect (not proven).
(The `primaryPHY:0` lead written here first was wrong: the parameter builder turns 0 into 1, and the trace below shows
Primary PHY 01, Secondary PHY 02.)

Reverted 2026-11-04 17:07 (`Kexts/BtLeDlFix.kext` = the build from before the advertising patches, from
`Kexts-backup/BtLeDlFix.kext.20261104-152640`; the f7d0263 build kept as `Kexts-backup/BtLeDlFix.kext.20261104-170738-f7d0263`).
The owner asked for the cause to be found and fixed instead of the revert.

**Third cause, from an HCI trace (2026-11-04 evening).** Method, no reboot needed: AMFI is off on this Mac
(`amfi_get_out_of_my_way=0x1`), so a copy of the x86_64 slice of bluetoothd with the patches applied as bytes, ad-hoc
signed with the original entitlements, can replace the system job until the next reboot (`launchctl bootout
system/com.apple.bluetoothd`, then `launchctl bootstrap` a copy of its plist pointing at the copy). HCI file traces are
turned on by `/Library/Preferences/com.apple.MobileBluetooth.debug.plist` = `HCITraces = {StackDebugEnabled, HCIFileTraces,
HIDTrace = true}`; bluetoothd then writes `/private/var/root/Library/Logs/Bluetooth/bluetoothd-hci-latest.pklg`
(PacketLogger format: u32 BE length, u64 BE time, u8 type 0 cmd / 1 evt / 2-3 ACL, payload).

With patches 2 and 3 the trace shows:

```
CMD 2035 00 76e8f9096c56   st=42   set random address, set 0  - sent at stack start, before set 0 exists
CMD 2035 02 9a5f1f117168   st=42   set random address, set 2  - same
... Finder AirDrop opened ...
CMD 2036 02 1000 ... 01 ...         st=00   ext adv parameters, set 2, own address type 1 (random)
CMD 2037 02 03 01 1b 02011a17ff4c000512...  st=00   AirDrop data (4C 00 05)
CMD 2039 01 01 02 0000 00           st=12   enable set 2 -> 718
CMD 2039 00 00 (x3), 2036, CMD 2037 00 ... st=42 (set 0 gone) -> 766, and again, ~3 loops per ms burst
```

Before Enable the stack compares the set's address with a 7-byte-per-set cache (`__text+0x1f7a1d`, table at
`0x100b2fe40`) and sends 0x2035 only when they differ. The completion callback (`__text+0x1f7ad3`) ignores the status, so
the 0x42 answers at stack start still fill the cache; when set 2 is created and enabled later the cache matches, the
address is never written to it and Enable fails with 0x12. Set 3 works because its 0x2035 comes after its 0x2036.
In one minute with AirDrop open: 2304× 718, 2299× 766, each loop ~8 HCI commands including disable-all. (That flood was first blamed
for the dead BLE mouse; wrong, see "BLE mouse connected but dead" below.)

**Fix: fourth patch in `kexts/BtLeDlFix`:** drop the "address unchanged" branch, so 0x2035 is sent after the set exists,
before every Enable (one extra command per enable; the set is disabled then, so the command is allowed):

```
44 33 84 08 0e 0d 00 00 8b 54 3a 03 33 94 08 11 0d 00 00 44 09 c2 0f 84 80 00 00 00
                                                  -> ... 44 09 c2 66 0f 1f 44 00 00
```

Pattern unique in 25G241, inside one page; `-btledlfxnoadv` turns off all three advertising patches.

**Live result (swapped bluetoothd with patches 2-4, 2026-11-04 23:37).** Every 0x2035 / 0x2036 / 0x2037 / 0x2039 after
stack start answers 0x00 (only the two early 0x2035 still get 0x42, harmless now). The AirDrop set is enabled:
`2037 02 ... ff4c0005 ...` st=00, `2035 02 ...` st=00, `2039 010102000000` st=00. No `result 718` / `result 766` in the log;
after the first seconds the HCI command rate drops to scan on/off only (~3 commands / 10 s). The mouse could not be checked
this way: the ad-hoc-signed copy is refused the System keychain item that holds the pairing keys (`Failed to query keychain
for device Public C6:54:32:2F:69:01 with result -25308`); the kext patches the signed system binary, so that does not
apply after a reboot.

Staged 2026-11-04 23:43: `Kexts/BtLeDlFix.kext` = patches 1-4; the build that was there (no advertising patches) is
`Kexts-backup/BtLeDlFix.kext.20261104-234307-preadv`.

**Boot 2026-11-05 10:24 (bbac9e0): advertising works, mouse dead.** No 718 / 766; every advertising command st=00. The
mouse connects and encrypts and sends HID notifications (908 ATT notifications on handle 0x001d in the HCI trace), but the
Mac sends nothing back except a rejected connection-parameter request: no MTU exchange, no service discovery.

### BLE mouse connected but dead (found 2026-11-05)

`log show` per boot, `LinkReady took` after `Remote Features:`: present only on the 17:30 boot (no advertising patches),
missing on 15:53, 16:53 and 2026-11-05 10:28 (all builds from this branch). Without LE Link Ready bluetoothd never sends
BTLEServer the connection-ready event, so no GATT discovery runs; the mouse's GATT database was not cached this boot, so
every notification is dropped (`No database exists for device`). On earlier no-adv boots the database was cached and the
link became ready anyway.

Cause: the link-ready step after LE Read Remote Features waits for an LE Data Length Change event when both sides
support EDL and the chipset is not 8. The MT7925 never sends that event. The 25G241 code is `call chipset;
cmp eax, 8; setne al; and al, r12b; mov [rbp-0x41], al`. The working ESP build v1.1.0 turned `cmp eax, 8` into
`cmp eax, eax; nop` (seen in its binary and in the bluetoothd pages it patched); its source was never committed, so the
advertising builds (v1.0.0 from this branch) lost it. The advertising patches themselves are not involved.

**Fix (v1.2.0):** the 25G241 pattern is back in `BtLeDlFix` next to the 15.8.1 one:

```
83 f8 08 0f 95 c0 44 20 e0 88 45 bf   ->   39 c0 90 0f 95 c0 44 20 e0 88 45 bf
```

Unique in 25G241. Staged 2026-11-05 10:36: `Kexts/BtLeDlFix.kext` = v1.2.0 (length wait 15.8.1 + 26, advertising patches
2-4); the bbac9e0 build is `Kexts-backup/BtLeDlFix.kext.20261105-103645-bbac9e0`.

**Result, boot 2026-11-05 10:42 (v1.2.0, 29db2ea): BLE mouse works** (owner confirmed). Log: `btledlfix (1.2.0)` loaded,
`LinkReady took 180ms from connectionComplete`, no `No database exists`, `enableBTSpecAdvertisingSets=1`, 0× 718 / 766,
6× `Started advertising successfully`. Still to check: `4C 00 05` on air (nRF Connect) and iPhone ↔ Mac AirDrop.

### Next-boot test

1. BLE mouse: move it; the pointer must follow, with Finder > AirDrop open and closed. Log check (expect `LinkReady took`
   and `LE Link Ready` after `Remote Features:`, no `No database exists`):
   ```
   /usr/bin/log show --last boot --style compact --predicate 'process == "bluetoothd" AND (eventMessage CONTAINS "LinkReady took" OR eventMessage CONTAINS "Remote Features:" OR eventMessage CONTAINS "No database exists")' | tail -5
   ```
2. Bluetooth log (expect `enableBTSpecAdvertisingSets=1` and 0 for 718 / 766):
   ```
   /usr/bin/log show --last boot --style compact --predicate 'process == "bluetoothd"' | grep -E "BT Spec advertisingsets|enableBTSpecAdvertisingSets|InitMultipleAdvertising|Stack is not started" | head
   /usr/bin/log show --last 5m --style compact --predicate 'process == "bluetoothd"' | grep -o -E "result 718|result 766|Started advertising successfully" | sort | uniq -c
   ```
3. `kextstat | grep btledl` shows 1.2.0.
4. Open Finder > AirDrop ("Everyone for 10 minutes"). On Android, nRF Connect scanner, filter manufacturer `0x004C`, RSSI
   ≥ -60: the Mac (random address, changes) must show `4C 00 05 …` (AirDrop).
5. Then retry AirDrop iPhone ↔ Mac (AWDL side: docs/ROADMAP-AWDL.md §10.38-10.41).

The HCI trace is still on (`/Library/Preferences/com.apple.MobileBluetooth.debug.plist`); read it with `sudo cp
/private/var/root/Library/Logs/Bluetooth/bluetoothd-hci-latest.pklg ~/x.pklg`. Remove the plist to stop tracing.

Undo: boot-arg `-btledlfxnoadv` (advertising patches off, length patches stay on), or
`rm -rf /Volumes/ESP/EFI/OC/Kexts/BtLeDlFix.kext && cp -R /Volumes/ESP/EFI/OC/Kexts-backup/BtLeDlFix.kext.20261104-152640 /Volumes/ESP/EFI/OC/Kexts/BtLeDlFix.kext`
(the v1.1.0 build: mouse works, no advertising), then reboot.

## Pieces

| Piece | What it does |
|---|---|
| `kexts/MtkBtFw` | Matches interface 0 of 0489:e11e, opens it, downloads `fw/BT_RAM_CODE_MT7925_1_1_hdr.bin` (embedded) and turns the Bluetooth function on, then closes the interface again. The procedure is Linux `drivers/bluetooth/btmtk.c` (`btmtk_usb_setup`, `btmtk_setup_firmware_79xx`, `btmtk_usb_hci_wmt_sync`): WMT commands as HCI vendor command 0xfc6f on the control endpoint, WMT events polled with a vendor IN request (bRequest 1, wValue 0x30), sections in 250-byte blocks, endpoint reset option 0x74011890 = 0x00010001, WMT FUNC_CTRL 1. Sections the chip reports as already loaded (warm reboot from Windows/Linux) are skipped. |
| `BlueToolFixup.kext` 2.7.2 | From [acidanthera/BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM/releases) (needs Lilu). Required on macOS 12+ so `bluetoothd` accepts a non-Apple controller. Not in this repository; copy it from the release zip. |
| `kexts/BtLeDlFix` | Lilu plugin, binary patches of `bluetoothd`: a BLE link does not wait for an HCI LE Data Length Change event the MT7925 never sends (macOS 15), and BT 5.0 advertising sets are enabled for the third-party controller, with each set's random address written before it is enabled, so LE advertising (AirDrop, Nearby) works (macOS 26, see above). Install: `EFI/OC/Kexts/BtLeDlFix.kext` + a `Kernel/Add` entry after Lilu. |
| NVRAM `7C436110-...:bluetoothExternalDongleFailed` = `00`, `bluetoothInternalControllerInfo` = 14 x `00` | Also listed in NVRAM/Delete so OpenCore writes them every boot (BrcmPatchRAM README: needed for non-Broadcom controllers). |

## Checking it

```
ioreg -rc MtkBtFw -l | grep -E '"bt-state"|"bt-log"'      # loading / ready / error, and every step
system_profiler SPBluetoothDataType | head -12               # a real address and chipset instead of the placeholder
```

The kexts' `IOLog` output does not reach `dmesg` or the unified log on this machine, so `bt-log` is the record. If two
controllers end up with the same address, BrcmPatchRAM documents the boot-arg `-btlfxallowanyaddr`.

## Undo

Set `Kernel/Add/[BundlePath=BtLeDlFix.kext]/Enabled` to false to undo only the BLE patch. Set `Kernel/Add/[BundlePath=MtkBtFw.kext]/Enabled` and `.../BlueToolFixup.kext/Enabled` to false (or restore
`EFI/OC/config.plist.bt-<time>` on the ESP).
